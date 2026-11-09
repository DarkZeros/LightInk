#include "trace.h"

#if CONFIG_LIGHTINK_TRACE

#include <cstdio>
#include <cstring>

#include "soc/gpio_reg.h"
#include "hal/uart_ll.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_wifi.h"
#if CONFIG_BT_ENABLED
#include "esp_bt.h"
#endif

#include "hardware.h"
#include "power.h"
#include "settings.h"

// Frame: 00 A5 5A | type | len (u16 LE) | payload | fletcher16 (u16 LE) over type..payload
// Text logs never contain 0x00, so the viewer can split frames from logs.
// Every event starts with t (u32 LE): us since app start, or since chip wake in the stub.
namespace {
  enum Type : uint8_t {
    kBoot = 1,       // t, cause, scripted (0 no, 1 button, 2 idle), btn, step, steps
    kStub = 2,       // t, stub wakeup cause (u32, RTC_CNTL bits)
    kSpi = 3,        // dc=1, data bytes...
    kButton = 4,     // t, btn, scripted
    kSleep = 5,      // t, scriptArmed, stubArmed, ui depth (i8), power mask, rail,
                     //    wifi mode (0xFF not init), bt status (0xFF none), battery mV (u16),
                     //    trace UART stall recoveries since power-on
    kScriptEnd = 6,  // t
    kSpiCmd = 7,     // t, command bytes... (DC low)
    kPower = 8,      // t, flag, locked, mask after, rail
    kPeriph = 9,     // t, kind (1 vibrator, 2 speaker), count, u16 values...
    kWifi = 10,      // t, event, data...
  };

  enum WifiEvent : uint8_t {
    kWifiStart = 1,  // (subscribed, driver already started)
    kWifiStop = 2,
    kWifiConnected = 3,     // channel, rssi(i8)=0, ssid...
    kWifiDisconnected = 4,  // reason, rssi (i8)
    kWifiGotIp = 5,         // ip (4 bytes)
    kWifiNtp = 6,
  };

  constexpr char kScript[] = CONFIG_LIGHTINK_TRACE_SCRIPT;

  RTC_DATA_ATTR bool sInApp {false}; // App context: lock stdout so logs don't split frames
  RTC_DATA_ATTR uint8_t sScriptPos {0};
  RTC_DATA_ATTR uint8_t sScriptStep {0};
  RTC_DATA_ATTR bool sScriptArmed {false};
  RTC_DATA_ATTR bool sScriptEnded {false};

  // This boot only
  bool sScripted {false};
  uint8_t sScriptedBtn {0};

  // The frame writer exists twice from one inlined body: in RTC fast memory for the
  // wake stub (no library calls, no const tables in flash), and in flash for the app,
  // where Power::lock may run on either core (ESP32 APP CPU can't run RTC fast code)
  // A byte drains in 87 us at 115200: a FIFO full for this many polls (~20 ms) means
  // TX stopped, e.g. auto light sleep left the UART forced to XOFF
  constexpr uint32_t kStallSpins = 200'000;
  RTC_DATA_ATTR uint8_t sStalls {0}; // Recoveries since power-on, reported at sleep

  struct Writer {
    uint16_t s1 {0}, s2 {0};
    bool ok {true}; // Rest of the frame dropped after an unrecoverable stall
  };

  // Never hangs: forces XON once, then drops the frame
  FORCE_INLINE_ATTR void putByte(uint8_t b, Writer& w) {
    if (!w.ok)
      return;
    for (uint32_t spin = 0; uart_ll_get_txfifo_len(&UART0) == 0; spin++) {
      if (spin == kStallSpins) {
        uart_ll_force_xon(UART_NUM_0);
        if (sStalls < 255)
          sStalls++;
      } else if (spin > 2 * kStallSpins) {
        w.ok = false;
        return;
      }
    }
    uart_ll_write_txfifo(&UART0, &b, 1);
  }

  FORCE_INLINE_ATTR void putSum(uint8_t b, Writer& w) {
    putByte(b, w);
    w.s1 = (w.s1 + b) % 255;
    w.s2 = (w.s2 + w.s1) % 255;
  }

  FORCE_INLINE_ATTR void emitBody(uint8_t type, const uint8_t* a, uint16_t alen, const uint8_t* b, uint16_t blen) {
    Writer w;
    const uint16_t len = alen + blen;
    putByte(0x00, w);
    putByte(0xA5, w);
    putByte(0x5A, w);
    putSum(type, w);
    putSum(uint8_t(len), w);
    putSum(uint8_t(len >> 8), w);
    for (uint16_t i = 0; i < alen; i++)
      putSum(a[i], w);
    for (uint16_t i = 0; i < blen; i++)
      putSum(b[i], w);
    const uint8_t s1 = w.s1, s2 = w.s2;
    putByte(s1, w);
    putByte(s2, w);
  }

  RTC_IRAM_ATTR void emitStub(uint8_t type, const uint8_t* a, uint16_t alen, const uint8_t* b, uint16_t blen) {
    emitBody(type, a, alen, b, blen);
    // The stub may sleep at any point: wait for the last bits, bounded like putByte
    for (uint32_t spin = 0; !uart_ll_is_tx_idle(&UART0) && spin < 2 * kStallSpins; spin++);
  }

  void emitApp(uint8_t type, const uint8_t* a, uint16_t alen, const uint8_t* b, uint16_t blen) {
    fflush(stdout);
    flockfile(stdout);
    emitBody(type, a, alen, b, blen);
    funlockfile(stdout);
  }

  // App or stub context
  FORCE_INLINE_ATTR void emit(uint8_t type, const uint8_t* a, uint16_t alen, const uint8_t* b = nullptr, uint16_t blen = 0) {
    if (sInApp)
      emitApp(type, a, alen, b, blen);
    else
      emitStub(type, a, alen, b, blen);
  }

  FORCE_INLINE_ATTR void putU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++)
      p[i] = uint8_t(v >> (8 * i));
  }

  // us since app start, or since the chip woke up (stub, CPU cycles)
  FORCE_INLINE_ATTR uint32_t traceTime() {
    if (sInApp)
      return uint32_t(esp_timer_get_time());
    return esp_cpu_get_cycle_count() / esp_rom_get_cpu_ticks_per_us();
  }

  uint8_t parse(char c) {
    switch (c) {
      case 'M': case 'm': return 0b0100; // Touch::MENU
      case 'U': case 'u': return 0b0010; // Touch::UP
      case 'D': case 'd': return 0b0001; // Touch::DOWN
      case 'B': case 'b': case 'L': case 'l': return 0b1000; // Touch::BACK / LIGHT
      case '.': return 0;
      default: return 0xFF; // Not a step
    }
  }

  // Next step position at or after pos, or the script end
  uint8_t nextStep(uint8_t pos) {
    while (pos < sizeof(kScript) - 1 && parse(kScript[pos]) == 0xFF)
      pos++;
    return pos;
  }

  uint8_t stepCount() {
    uint8_t n = 0;
    for (uint8_t pos = nextStep(0); pos < sizeof(kScript) - 1; pos = nextStep(pos + 1))
      n++;
    return n;
  }

  void emitWifi(uint8_t event, const uint8_t* data = nullptr, uint8_t len = 0) {
    uint8_t p[5];
    putU32(p, traceTime());
    p[4] = event;
    emit(kWifi, p, sizeof(p), data, len);
  }

  void onWifiEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
      emitWifi(kWifiStop);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
      auto& e = *static_cast<wifi_event_sta_connected_t*>(data);
      uint8_t p[2 + 32] = {e.channel, 0};
      const uint8_t n = e.ssid_len < 32 ? e.ssid_len : 32;
      memcpy(p + 2, e.ssid, n);
      emitWifi(kWifiConnected, p, 2 + n);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
      auto& e = *static_cast<wifi_event_sta_disconnected_t*>(data);
      const uint8_t p[2] = {e.reason, uint8_t(e.rssi)};
      emitWifi(kWifiDisconnected, p, sizeof(p));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
      auto& e = *static_cast<ip_event_got_ip_t*>(data);
      uint8_t p[4];
      putU32(p, e.ip_info.ip.addr); // Network order in memory = a.b.c.d
      emitWifi(kWifiGotIp, p, sizeof(p));
    }
  }
}

namespace Trace {
  RTC_IRAM_ATTR void spiImpl(const void* data, uint32_t len) {
    const uint8_t dc = (REG_READ(GPIO_OUT_REG) >> HW::Display::Dc) & 1;
    if (dc) {
      emit(kSpi, &dc, 1, static_cast<const uint8_t*>(data), len);
    } else {
      uint8_t t[4];
      putU32(t, traceTime());
      emit(kSpiCmd, t, sizeof(t), static_cast<const uint8_t*>(data), len);
    }
  }

  RTC_IRAM_ATTR void stubWakeImpl(uint32_t cause) {
    sInApp = false;
    uint8_t p[8];
    putU32(p, traceTime());
    putU32(p + 4, cause);
    emit(kStub, p, sizeof(p));
  }

  esp_sleep_wakeup_cause_t wakeCauseImpl(esp_sleep_wakeup_cause_t cause) {
    // Only the timer wake armed by the script is a step, real touches still work
    if (sScriptArmed && cause == ESP_SLEEP_WAKEUP_TIMER) {
      const auto pos = nextStep(sScriptPos);
      if (pos < sizeof(kScript) - 1) {
        sScripted = true;
        sScriptedBtn = parse(kScript[pos]);
        sScriptPos = pos + 1;
        sScriptStep++;
        if (sScriptedBtn)
          cause = ESP_SLEEP_WAKEUP_TOUCHPAD;
      }
    }
    sScriptArmed = false;

    // HW v4 drops the rail to ~1.9 V when no lock is held: UART0 TX then swings too
    // low for the USB-serial adapter (bit slips), and the long busy loops of a trace
    // at that voltage (below the ESP32's 2.3 V minimum) froze the watch. Keep the rail
    // high while the app is awake, released in sleepImpl. Not reported as a lock.
    Power::lock(Power::Flag::Trace);
    esp_rom_delay_us(2'000); // Rail ramp, like the wake stub's display lock

    // Static init time, nothing else is logging yet
    uint8_t p[9];
    putU32(p, uint32_t(esp_timer_get_time()));
    p[4] = uint8_t(cause);
    p[5] = uint8_t(sScripted ? (sScriptedBtn ? 1 : 2) : 0);
    p[6] = sScriptedBtn;
    p[7] = sScriptStep;
    p[8] = stepCount();
    emitStub(kBoot, p, sizeof(p), nullptr, 0); // No stdio lock this early
    sInApp = true;
    return cause;
  }

  uint8_t touchImpl(uint8_t btn) {
    if (sScripted)
      btn = sScriptedBtn;
    uint8_t p[6];
    putU32(p, traceTime());
    p[4] = btn;
    p[5] = sScripted;
    emit(kButton, p, sizeof(p));
    return btn;
  }

  void sleepImpl() {
    const bool stubArmed = esp_get_deep_sleep_wake_stub() != nullptr;
    const bool pending = nextStep(sScriptPos) < sizeof(kScript) - 1;
    if (pending) {
      // Wake the full app for the next step, not the minute-update stub
      if (stubArmed)
        esp_set_deep_sleep_wake_stub(NULL);
      esp_sleep_enable_timer_wakeup(CONFIG_LIGHTINK_TRACE_STEP_MS * 1000ULL);
      sScriptArmed = true;
    } else if (!sScriptEnded && sizeof(kScript) > 1) {
      sScriptEnded = true;
      uint8_t t[4];
      putU32(t, traceTime());
      emit(kScriptEnd, t, sizeof(t));
    }

    // Anything left powered here costs current for the whole sleep
    wifi_mode_t wifiMode;
    uint8_t bt = 0xFF;
#if CONFIG_BT_ENABLED
    bt = esp_bt_controller_get_status();
#endif
    uint8_t p[14];
    putU32(p, traceTime());
    p[4] = pending;
    p[5] = stubArmed && !pending;
    p[6] = uint8_t(kSettings.mUi.mDepth);
    // Locks and rail as the sleep will have them, once the trace's own lock is released
    const uint16_t locks = Power::status() & ~Power::Flag::Trace;
    bool rail = Power::current();
    if constexpr (HW::kVoltageSelectPin != uint8_t(-1))
      rail = locks || !HW::kHasLowVoltage || !kSettings.mPower.mLowVoltage;
    p[7] = locks;
    p[8] = rail;
    p[9] = esp_wifi_get_mode(&wifiMode) == ESP_OK ? uint8_t(wifiMode) : 0xFF;
    p[10] = bt;
    p[11] = uint8_t(kSettings.mBattery.mVoltage);
    p[12] = uint8_t(kSettings.mBattery.mVoltage >> 8);
    p[13] = sStalls;
    emit(kSleep, p, sizeof(p));
    sInApp = false;

    // Last frame out before the rail drops
    for (uint32_t spin = 0; !uart_ll_is_tx_idle(&UART0) && spin < 2 * kStallSpins; spin++);
    Power::unlock(Power::Flag::Trace);
  }

  RTC_SLOW_ATTR void powerImpl(uint8_t flag, bool locked, uint8_t mask, bool rail) {
    if (flag == Power::Flag::Trace)
      return; // The trace's own lock, see wakeCauseImpl
    mask &= ~Power::Flag::Trace;
    uint8_t p[8];
    putU32(p, traceTime());
    p[4] = flag;
    p[5] = locked;
    p[6] = mask;
    p[7] = rail;
    emit(kPower, p, sizeof(p));
  }

  void periphImpl(uint8_t kind, const uint16_t* values, uint8_t count) {
    uint8_t p[6 + 64];
    putU32(p, traceTime());
    p[4] = kind;
    p[5] = count;
    for (uint8_t i = 0; i < count; i++) {
      p[6 + 2 * i] = uint8_t(values[i]);
      p[7 + 2 * i] = uint8_t(values[i] >> 8);
    }
    emit(kPeriph, p, 6 + 2 * count);
  }

  void wifiStartedImpl() {
    static bool sRegistered = false;
    if (!sRegistered) {
      sRegistered = true;
      esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifiEvent, nullptr);
      esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onWifiEvent, nullptr);
    }
    emitWifi(kWifiStart);
  }

  void ntpSyncedImpl() {
    emitWifi(kWifiNtp);
  }
};

#endif
