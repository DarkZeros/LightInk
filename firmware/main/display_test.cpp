#include "display_test.h"

#if CONFIG_LIGHTINK_DISPLAY_TEST

#include <algorithm>
#include <cstring>
#include <iterator>

#include <Arduino.h>
#include <Adafruit_GFX.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#include "hardware.h"
#include "spi.h"
#include "uspi.h"
#include "power.h"
#include "light.h"
#include "touch.h"
#include "settings.h"
#include "trace.h"
#include "lut.h"

// Wake cause after the trace script is applied (core.cpp)
extern esp_sleep_wakeup_cause_t wakeup_reason;

// All coordinates in "view" space are what the user sees (GFX rotation applied),
// "RAM" space is the controller RAM (x in pixels, always byte aligned here).
// Pixel bit 1 = white, 0 = black, MSB is the lowest x of the byte.
namespace {
  constexpr char TAG[] = "dtest";
  constexpr int kW = 200, kH = 200;

  struct Box { int x, y, w, h; };

  // Test boxes A..F in view space, 48x48, all edges multiples of 8 in any rotation
  constexpr Box kView[6] = {
    {16, 32, 48, 48}, {80, 32, 48, 48}, {144, 32, 48, 48},
    {16, 112, 48, 48}, {80, 112, 48, 48}, {144, 112, 48, 48},
  };
  enum { A, B, C, D, E, F };

  enum Pat : uint8_t { kBlack, kWhite, kHStripe, kVStripe, kChecker, kHalf, kFirstPx };

  uint8_t pattern(Pat p, int row, int col) {
    switch (p) {
      case kBlack: return 0x00;
      case kWhite: return 0xFF;
      case kHStripe: return (row / 4) & 1 ? 0xFF : 0x00; // 4 lines black, 4 white
      case kVStripe: return 0x0F;                        // 4 px black (low x), 4 px white
      case kChecker: return ((row / 8) + col) & 1 ? 0x00 : 0xFF;
      case kHalf: return col < 3 ? 0x00 : 0xFF;          // first 3 bytes written black
      case kFirstPx: return col == 0 ? 0x7F : 0xFF;      // MSB pixel of the first byte only
    }
    return 0xFF;
  }

  RTC_DATA_ATTR struct State {
    int8_t sel {0};
    bool inResult {false};
    bool menuOnGlass {false}; // Last refresh drew the menu with the standard registers
  } sState;

  // -------------------------------------------------------------------------
  // Raw controller access, everything goes through uSpi so the trace sees it

  void begin(uint8_t c) {
    gpio_set_level((gpio_num_t)HW::Display::Cs, 0);
    uSpi::command(c);
  }

  void send(const uint8_t* p, size_t n) {
    alignas(4) uint8_t bounce[64]; // uSpi reads 32 bit words
    while (n) {
      const size_t c = n < sizeof(bounce) ? n : sizeof(bounce);
      memcpy(bounce, p, c);
      uSpi::write(bounce, c);
      p += c;
      n -= c;
    }
  }

  void end() {
    gpio_set_level((gpio_num_t)HW::Display::Cs, 1);
  }

  void cmd(uint8_t c, std::initializer_list<uint8_t> d = {}) {
    begin(c);
    send(d.begin(), d.size());
    end();
  }

  int waitBusy() {
    const auto t0 = esp_timer_get_time();
    delay(1); // Busy rises a bit after the command
    while (gpio_get_level((gpio_num_t)HW::Display::Busy)) {
      if (esp_timer_get_time() - t0 > 15'000'000) {
        ESP_LOGE(TAG, "busy timeout!");
        break;
      }
      delay(1);
    }
    return (esp_timer_get_time() - t0) / 1000;
  }

  // Driver style (T78/T79): 0x22 only sent when it changes, as Display/uSpi do
  bool sDriverStyle = false;
  int sUpd2 = -1; // Last 0x22 sent, -1 = unknown (after SWRESET)

  void hwReset() {
    gpio_set_level((gpio_num_t)HW::Display::Res, 0);
    delay(1);
    gpio_set_level((gpio_num_t)HW::Display::Res, 1);
    delay(10);
    waitBusy();
  }

  void swReset() {
    cmd(0x12);
    waitBusy();
    sUpd2 = -1;
  }

  // 0x10 deep sleep mode (1 = keep RAM, 3 = mode 2) then HW reset to wake it up
  void sleepAndReset(uint8_t mode = 0x01) {
    cmd(0x10, {mode});
    delay(50);
    hwReset();
  }

  void window(int xs, int xe, int ys, int ye) {
    cmd(0x44, {uint8_t(xs), uint8_t(xe)});
    cmd(0x45, {uint8_t(ys), uint8_t(ys >> 8), uint8_t(ye), uint8_t(ye >> 8)});
  }

  void counter(int x, int y) {
    cmd(0x4E, {uint8_t(x)});
    cmd(0x4F, {uint8_t(y), uint8_t(y >> 8)});
  }

  void fullWindow() {
    window(0, kW / 8 - 1, 0, kH - 1);
    counter(0, 0);
  }

  // Window = RAM box, counter at its first line + row
  void setBox(const Box& r, int row = 0) {
    window(r.x / 8, (r.x + r.w - 1) / 8, r.y, r.y + r.h - 1);
    counter(r.x / 8, r.y + row);
  }

  // Send n bytes of a pattern to the current command, rowLen bytes per row
  void stream(size_t n, Pat p, int rowLen = 6, int firstRow = 0) {
    uint8_t buf[64];
    size_t i = 0;
    while (i < n) {
      size_t c = 0;
      for (; c < sizeof(buf) && i < n; c++, i++)
        buf[c] = pattern(p, firstRow + i / rowLen, i % rowLen);
      send(buf, c);
    }
  }

  void fill(uint8_t ram, const Box& r, Pat p) {
    setBox(r);
    begin(ram);
    stream(r.w / 8 * r.h, p, r.w / 8);
    end();
  }

  struct Timing { uint8_t upd; const char* what; int ms; };
  Timing sTimings[12];
  int sNumTimings = 0;

  void activate(uint8_t upd, const char* what) {
    begin(0x20);
    end();
    const int ms = waitBusy();
    ESP_LOGE(TAG, "  refresh 0x22=0x%02X %s: busy %d ms", upd, what, ms);
    if (sNumTimings < 12)
      sTimings[sNumTimings++] = {upd, what, ms};
  }

  // keepWindow: don't reset window/counter to full screen before the refresh
  void refresh(uint8_t upd, bool keepWindow = false, const char* what = "") {
    if (!keepWindow)
      fullWindow();
    if (!sDriverStyle || sUpd2 != upd)
      cmd(0x22, {upd});
    sUpd2 = upd;
    activate(upd, what);
  }

  void loadLut(const LUT& lut) {
    const auto a = lut.get();
    begin(0x32);
    send(a.data(), a.size());
    end();
    if (lut.eopt) cmd(0x3F, {*lut.eopt});
    if (lut.vgh) cmd(0x03, {*lut.vgh});
    if (lut.vsh1_vsh2_vsl) cmd(0x04, {(*lut.vsh1_vsh2_vsl)[0], (*lut.vsh1_vsh2_vsl)[1], (*lut.vsh1_vsh2_vsl)[2]});
    if (lut.vcom) cmd(0x2C, {*lut.vcom});
  }

  // -------------------------------------------------------------------------
  // Base picture: header, labelled framed boxes, legend. Boxes are white.

  GFXcanvas1* sCanvas;

  uint8_t rot() { return kSettings.mDisplay.mRotation & 3; }

  void toRam(int x, int y, int& rx, int& ry) {
    switch (rot()) {
      case 1: rx = kW - 1 - y; ry = x; break;
      case 2: rx = kW - 1 - x; ry = kH - 1 - y; break;
      case 3: rx = y; ry = kH - 1 - x; break;
      default: rx = x; ry = y; break;
    }
  }

  void toView(int rx, int ry, int& x, int& y) {
    switch (rot()) {
      case 1: x = ry; y = kW - 1 - rx; break;
      case 2: x = kW - 1 - rx; y = kH - 1 - ry; break;
      case 3: x = kH - 1 - ry; y = rx; break;
      default: x = rx; y = ry; break;
    }
  }

  template<typename Fn>
  Box mapBox(const Box& b, Fn fn) {
    int x0, y0, x1, y1;
    fn(b.x, b.y, x0, y0);
    fn(b.x + b.w - 1, b.y + b.h - 1, x1, y1);
    return {std::min(x0, x1), std::min(y0, y1), abs(x1 - x0) + 1, abs(y1 - y0) + 1};
  }

  Box ram(int box) { return mapBox(kView[box], toRam); }

  struct Test;
  const Test* sTest;
  int sTestNum;

  struct Layout {
    struct Mark { uint8_t box; bool cols; uint8_t from, count; char c; };
    uint8_t boxes {0};
    const char* caption[6] {};
    Mark marks[4] {};
    uint8_t numMarks {0};
    bool modeSwitch {true}; // 0xD8 update after the full refresh, as the driver does

    Layout& box(int i, const char* cap = "") { boxes |= 1 << i; caption[i] = cap; return *this; }
    // Lines (RAM rows relative to the box) / columns (RAM px) of a box, with a tag
    Layout& rows(int i, int from, int n, char c) { marks[numMarks++] = {uint8_t(i), false, uint8_t(from), uint8_t(n), c}; return *this; }
    Layout& cols(int i, int from, int n, char c) { marks[numMarks++] = {uint8_t(i), true, uint8_t(from), uint8_t(n), c}; return *this; }
    Layout& noSwitch() { modeSwitch = false; return *this; }
  };

  // -------------------------------------------------------------------------
  // Test table

  struct Test {
    const char* name;      // Header line 1
    const char* question;  // Header line 2
    void (*fn)(int param);
    int param;
    const char* legend[4]; // Hypothesis -> expected picture
  };

  void drawBase(const Layout& l) {
    auto& c = *sCanvas;
    c.setRotation(rot());
    c.fillScreen(1);
    c.setTextColor(0);
    c.setTextSize(1);
    c.setTextWrap(false);
    c.setCursor(2, 2);
    c.printf("T%02d %s", sTestNum, sTest->name);
    c.setCursor(2, 12);
    c.print(sTest->question);
    for (int i = 0; i < 6; i++) {
      if (!(l.boxes >> i & 1))
        continue;
      const auto& v = kView[i];
      c.drawRect(v.x - 2, v.y - 2, v.w + 4, v.h + 4, 0);
      c.setCursor(v.x, v.y - 10);
      c.printf("%c %s", 'A' + i, l.caption[i]);
    }
    for (int i = 0; i < l.numMarks; i++) {
      const auto& m = l.marks[i];
      const auto& v = kView[m.box];
      const auto r = ram(m.box);
      if (m.cols) {
        const auto s = mapBox(Box{r.x + m.from, r.y, m.count, r.h}, toView);
        c.fillRect(s.x, v.y + v.h + 4, s.w, 2, 0);
        c.setCursor(s.x + s.w / 2 - 2, v.y + v.h + 8);
      } else {
        const auto s = mapBox(Box{r.x, r.y + m.from, r.w, m.count}, toView);
        c.fillRect(v.x - 7, s.y, 2, s.h, 0);
        c.setCursor(v.x - 14, s.y + s.h / 2 - 3);
      }
      c.print(m.c);
    }
    for (int i = 0; i < 4 && sTest->legend[i]; i++) {
      c.setCursor(2, 164 + 9 * i);
      c.print(sTest->legend[i]);
    }
  }

  void writeCanvas(uint8_t ramCmd) {
    fullWindow();
    begin(ramCmd);
    send(sCanvas->getBuffer(), kW / 8 * kH);
    end();
  }

  // Registers Display::init sets after a SWRESET (not window/counter)
  void initRegs() {
    cmd(0x01, {0xC7, 0x00, 0x00});
    cmd(0x1A, {0x32, 0x00}); // Fixed 50C, like the driver (kFastUpdateTemp). Without it
                             // the OTP LUT is empty: a 92 ms update that drives nothing
    cmd(0x11, {0x03});
    cmd(0x3C, {0x05});
    cmd(0x21, {0x00});
  }

  // Known state: reset, init like Display::init, base picture in both buffers, full refresh
  void prepare(const Layout& l) {
    hwReset();
    swReset();
    initRegs();
    drawBase(l);
    writeCanvas(0x24);
    writeCanvas(0x26);
    refresh(0xD4, false, "prepare full");
    if (l.modeSwitch)
      refresh(0xD8, false, "prepare 2-buffer switch");
    sNumTimings = 0;
  }

  // --- Readout basics ---------------------------------------------------------

  void tFullShowsFront(int) {
    prepare(Layout().box(A, "fr0 bk1").box(B, "fr1 bk0").box(C, "frH bkV"));
    fill(0x24, ram(A), kBlack);
    fill(0x24, ram(B), kWhite);
    fill(0x26, ram(B), kBlack);
    fill(0x24, ram(C), kHStripe);
    fill(0x26, ram(C), kVStripe);
    refresh(0xD4);
  }

  void tPartialDiff(int noSwitch) {
    Layout l = Layout().box(A, "fr0 bk1").box(B, "fr0 bk0").box(C, "fr1 bk0").box(D, "frH bk1");
    if (noSwitch)
      l.noSwitch();
    prepare(l);
    fill(0x24, ram(A), kBlack);
    fill(0x24, ram(B), kBlack);
    fill(0x26, ram(B), kBlack);
    fill(0x26, ram(C), kBlack);
    fill(0x24, ram(D), kHStripe);
    refresh(0xDC);
  }

  // --- RAM copy after an update ----------------------------------------------
  // A: front black / back white, B: front white / back black, then update #1 (param),
  // then (if #1 displayed) front A := white, then a partial #2 reveals back vs front.
  // param bit 8: load the custom 2_1 LUT first.

  void tCopyProbe(int param) {
    const uint8_t upd = param & 0xFF;
    prepare(Layout().box(A, "fr0 bk1").box(B, "fr1 bk0"));
    if (param & 0x100)
      loadLut(SSD1681_LIGHTMYINK_CUSTOM_2_1);
    fill(0x24, ram(A), kBlack);
    fill(0x26, ram(B), kBlack);
    refresh(upd, false, "#1 probe");
    if (upd & 0x04)
      fill(0x24, ram(A), kWhite);
    refresh(0xDC, false, "#2 readout");
  }

  // Cheaper swap than D8 (T82-T85)? The driver runs 0xD8 (~92 ms BUSY, mostly the
  // analog power-on) before the first partial after a full refresh, only for its
  // swap. Does an update with the mode 2 bit but no analog swap too? As tCopyProbe,
  // but right after a full refresh like in the driver, with the probe's BUSY time
  // written on screen (front buffer only: both buffers hold the base picture there).
  void tSwapProbe(int upd) {
    prepare(Layout().box(A, "fr0 bk1").box(B, "fr1 bk0").noSwitch());
    fill(0x24, ram(A), kBlack);
    fill(0x26, ram(B), kBlack);
    refresh(upd, false, "probe");
    const int ms = sTimings[sNumTimings - 1].ms;
    auto& c = *sCanvas;
    c.setCursor(2, 100);
    c.printf("probe 0x%02X: busy %d ms", upd, ms);
    const auto band = mapBox(Box{0, 96, kW, 16}, toRam);
    setBox(band);
    begin(0x24);
    send(sCanvas->getBuffer() + band.y * (kW / 8), band.h * (kW / 8));
    end();
    refresh(0xDC, false, "readout");
  }

  void tRefreshWindow(int) {
    prepare(Layout().box(A, "fr0 bk1").box(B, "fr0 bk1"));
    fill(0x24, ram(A), kBlack);
    fill(0x24, ram(B), kBlack);
    setBox(ram(A));
    refresh(0xDC, true, "window = A");
  }

  void tCopyWindow(int) {
    prepare(Layout().box(A, "fr0 bk1").box(B, "fr0 bk1"));
    fill(0x24, ram(A), kBlack);
    fill(0x24, ram(B), kBlack);
    setBox(ram(A));
    refresh(0xDC, true, "#1 window = A");
    fill(0x24, ram(A), kWhite);
    fill(0x24, ram(B), kWhite);
    refresh(0xDC, false, "#2 full window");
  }

  // --- Address counters -----------------------------------------------------

  void tCounterShared(int) {
    prepare(Layout().box(A).rows(A, 0, 12, 'C').rows(A, 12, 12, 'S').rows(A, 24, 12, 'N'));
    const auto a = ram(A);
    setBox(a, 12);
    begin(0x24); stream(72, kBlack); end();
    begin(0x26); stream(72, kBlack); end(); // No counter set
    fill(0x24, a, kBlack);                  // Readout: back black stays white
    refresh(0xDC);
  }

  void tCounterAfterRefresh(int) {
    prepare(Layout().box(A).rows(A, 0, 24, 'W').rows(A, 24, 24, 'K'));
    setBox(ram(A));
    begin(0x24); stream(144, kBlack); end();
    refresh(0xDC, true, "#1");
    begin(0x24); stream(144, kVStripe); end(); // No window/counter set
    refresh(0xDC, true, "#2");
  }

  // param 0: 0x10 + HW reset, 1: 0x12 SWRESET
  void tCounterAfterReset(int param) {
    prepare(Layout().box(A).rows(A, 0, 24, 'W').rows(A, 24, 24, 'K'));
    setBox(ram(A));
    begin(0x24); stream(144, kBlack); end();
    if (param) {
      swReset();
      initRegs(); // Leaves window/counter alone
    } else {
      sleepAndReset();
    }
    begin(0x24); stream(144, kVStripe); end();
    refresh(0xDC, true);
  }

  // --- Retention across sleep / resets -----------------------------------------
  // param: 0x01 / 0x03 deep sleep mode + HW reset, 0x12 SWRESET (+ init registers),
  // bits 8+: seconds to stay in deep sleep

  void tRamKept(int param) {
    const uint8_t mode = param & 0xFF;
    const int seconds = param >> 8;
    prepare(Layout().box(A, "fr0 bk1"));
    fill(0x24, ram(A), kBlack);
    if (mode == 0x12) {
      swReset();
      initRegs();
    } else if (seconds) {
      cmd(0x10, {mode});
      for (int s = seconds; s > 0; s -= 10) {
        ESP_LOGE(TAG, "  in deep sleep 0x%02X, %d s left", mode, s);
        delay(std::min(s, 10) * 1000);
      }
      hwReset();
    } else {
      sleepAndReset(mode);
    }
    refresh(0xDC);
  }

  // PPK2: 20 s in deep sleep mode 1, then 20 s in mode 2 (display rail stays locked)
  void tSleepCurrent(int) {
    prepare(Layout().box(A, "fr0 bk1"));
    fill(0x24, ram(A), kBlack);
    for (uint8_t mode : {0x01, 0x03}) {
      ESP_LOGE(TAG, "  deep sleep mode 0x%02X for 20 s: measure now", mode);
      cmd(0x10, {mode});
      delay(20'000);
      hwReset();
    }
    refresh(0xDC);
  }

  // param 0: control (no reset), 1: across sleep + HW reset
  void tFrontInverse(int param) {
    prepare(Layout());
    cmd(0x21, {0x08});
    if (param)
      sleepAndReset();
    refresh(0xDC);
  }

  void tUpd2Kept(int) {
    prepare(Layout().box(A, "fr0 bk0"));
    fill(0x24, ram(A), kBlack);
    fill(0x26, ram(A), kBlack);
    fullWindow();
    cmd(0x22, {0xD4});
    sleepAndReset();
    activate(0, "0x20 only"); // No 0x22 sent
  }

  void tBorderKept(int) {
    prepare(Layout());
    cmd(0x3C, {0x02});
    sleepAndReset();
    refresh(0xD4);
  }

  void tLutKept(int param) {
    prepare(Layout().box(A, "fr0 bk1"));
    loadLut(SSD1681_LIGHTMYINK_CUSTOM_2_1);
    if (param)
      sleepAndReset();
    fill(0x24, ram(A), kBlack);
    refresh(0xCC, false, "custom LUT, no LUT load");
  }

  // param 0: control, 1: across sleep + HW reset
  void tEntryYFirst(int param) {
    prepare(Layout().box(A, "Y first").box(B, "X first").rows(A, 0, 8, 'X').cols(A, 0, 8, 'Y'));
    cmd(0x11, {0x07});
    if (param)
      sleepAndReset();
    setBox(ram(A));
    begin(0x24); stream(48, kBlack); end();
    cmd(0x11, {0x03});
    setBox(ram(B));
    begin(0x24); stream(48, kBlack); end();
    refresh(0xDC);
  }

  // --- Window / entry modes -------------------------------------------------

  void tEntryModes(int) {
    prepare(Layout().box(A, "X-Y-").box(B, "X+Y-").box(C, "X-Y+").box(D, "X+Y+")
                    .box(E, "X- 0F").box(F, "X+ 0F"));
    const struct { int box; uint8_t mode; Pat p; } runs[] = {
      {A, 0x00, kHalf}, {B, 0x01, kHalf}, {C, 0x02, kHalf}, {D, 0x03, kHalf},
      {E, 0x02, kVStripe}, {F, 0x03, kVStripe},
    };
    for (const auto& r : runs) {
      const auto b = ram(r.box);
      const int x0 = b.x / 8, x1 = (b.x + b.w - 1) / 8, y0 = b.y, y1 = b.y + b.h - 1;
      const bool xInc = r.mode & 1, yInc = r.mode & 2;
      cmd(0x11, {r.mode});
      window(xInc ? x0 : x1, xInc ? x1 : x0, yInc ? y0 : y1, yInc ? y1 : y0);
      counter(xInc ? x0 : x1, yInc ? y0 : y1);
      begin(0x24); stream(r.p == kHalf ? 48 : 96, r.p); end();
    }
    cmd(0x11, {0x03});
    refresh(0xDC);
  }

  // param 0: plain window, 1: 0x45 sent first with end high byte 1, then with 3 params
  void tWrap(int param) {
    prepare(Layout().box(A).rows(A, 0, 12, 'W'));
    const auto a = ram(A);
    setBox(a);
    if (param) {
      cmd(0x45, {uint8_t(a.y), 0, uint8_t(a.y + a.h - 1), 1});
      cmd(0x45, {uint8_t(a.y), 0, uint8_t(a.y + a.h - 1)});
      counter(a.x / 8, a.y);
    }
    begin(0x24);
    stream(288, kBlack);
    stream(72, kVStripe);
    end();
    refresh(0xDC);
  }

  void tCounterOutside(int) {
    prepare(Layout().box(A, "window").box(B, "counter"));
    const auto a = ram(A), b = ram(B);
    setBox(a);
    counter(b.x / 8, b.y);
    begin(0x24); stream(72, kBlack); end();
    refresh(0xDC);
  }

  void tWindowReversed(int) {
    prepare(Layout().box(A).box(B).box(C));
    const auto a = ram(A), c = ram(C);
    // X start > X end while incrementing, Y = box rows
    const int xs = std::max(a.x, c.x) / 8, xe = std::min(a.x, c.x) / 8;
    window(xs, xe, a.y, a.y + a.h - 1);
    counter(xs, a.y);
    begin(0x24); stream(144, kBlack); end();
    refresh(0xDC);
  }

  // Both counters at line 0, advance only the back buffer's (white write = no change),
  // swap with a refresh, then write 0x24 without setting the counter
  void tCounterFollowsSwap(int) {
    prepare(Layout().box(A).rows(A, 0, 12, 'R').rows(A, 12, 12, 'F'));
    setBox(ram(A));
    begin(0x26); stream(72, kWhite); end();
    refresh(0xDC, true, "#1 swap");
    begin(0x24); stream(72, kBlack); end();
    refresh(0xDC, true, "#2 readout");
  }

  // X- vs X+ with a single black pixel (MSB) in the first byte of every row
  void tBitOrder(int) {
    prepare(Layout().box(E, "X- 7F").box(F, "X+ 7F"));
    for (const auto& [box, mode] : {std::pair{E, uint8_t(0x02)}, std::pair{F, uint8_t(0x03)}}) {
      const auto b = ram(box);
      const int x0 = b.x / 8, x1 = (b.x + b.w - 1) / 8;
      const bool xInc = mode & 1;
      cmd(0x11, {mode});
      window(xInc ? x0 : x1, xInc ? x1 : x0, b.y, b.y + b.h - 1);
      counter(xInc ? x0 : x1, b.y);
      begin(0x24); stream(96, kFirstPx); end();
    }
    cmd(0x11, {0x03});
    refresh(0xDC);
  }

  // Which RAM does a full refresh show after partial ones? (the watch leaving a menu
  // with FAST/custom LUT for a FULL watchface kept showing the menu). After prepare,
  // n partial refreshes that change nothing (each swaps the buffers), then front A black and
  // a full 0xD4. param: n | 0x10 custom 2_1 partials | 0x20 0xD0 update before the
  // full (the driver before 5033876) | 0x40 A black in both buffers (candidate fix)
  void tFullAfterPartials(int param) {
    const int partials = param & 0xF;
    const bool custom = param & 0x10, d0 = param & 0x20, both = param & 0x40;
    prepare(Layout().box(A, both ? "fr0 bk0" : "fr0"));
    if (custom)
      loadLut(SSD1681_LIGHTMYINK_CUSTOM_2_1);
    for (int i = 0; i < partials; i++)
      refresh(custom ? 0xCC : 0xDC, false, "partial, no change");
    fill(0x24, ram(A), kBlack);
    if (both)
      fill(0x26, ram(A), kBlack);
    if (d0)
      refresh(0xD0, false, "no display, load OTP LUT");
    refresh(0xD4, false, "full");
  }

  // Same colour in both buffers over glass of the other colour: A white glass + BB,
  // B black glass + WW, controls C (black + BB) and D (white + WW).
  // param: number of partial refreshes, bit 8: custom 2_1 LUT instead of OTP
  void tSameColour(int param) {
    const int repeats = param & 0xFF;
    const bool custom = param & 0x100;
    prepare(Layout().box(A, "wBB").box(B, "kWW").box(C, "kBB").box(D, "wWW"));
    for (uint8_t r : {0x24, 0x26}) {
      fill(r, ram(B), kBlack);
      fill(r, ram(C), kBlack);
    }
    refresh(0xD4, false, "glass B, C black");
    for (uint8_t r : {0x24, 0x26}) {
      fill(r, ram(A), kBlack);
      fill(r, ram(B), kWhite);
    }
    if (custom)
      loadLut(SSD1681_LIGHTMYINK_CUSTOM_2_1);
    for (int i = 0; i < repeats; i++)
      refresh(custom ? 0xCC : 0xDC, false, "same colour");
  }

  // --- Update options -----------------------------------------------------------

  void tBackInverse(int) {
    prepare(Layout().box(A, "fr0 bk1"));
    fill(0x24, ram(A), kBlack);
    cmd(0x21, {0x80});
    refresh(0xDC);
  }

  void tFrontBypass(int) {
    prepare(Layout());
    cmd(0x21, {0x04});
    refresh(0xDC);
  }

  void tBackBypass(int) {
    prepare(Layout().box(A, "fr0 bk1"));
    fill(0x24, ram(A), kBlack);
    cmd(0x21, {0x40});
    refresh(0xDC);
  }

  void tBorder(int value) {
    prepare(Layout());
    cmd(0x3C, {uint8_t(value)});
    refresh(0xD4);
  }

  // --- RAM ping-pong (0x37) --------------------------------------------------
  // 0x37 Display Option, byte F bit 6 = "RAM ping-pong for display mode 2" per the
  // SSD1680/1681 datasheets (POR: disabled), Waveshare enables it with
  // 00 00 00 00 00 40 00 00 00 00 for partial refreshes. The swap measured in
  // T04-T09 may be that option, loaded from OTP.

  void displayOption(bool pingPong) {
    cmd(0x37, {0, 0, 0, 0, 0, uint8_t(pingPong ? 0x40 : 0x00), 0, 0, 0, 0});
  }

  // As T04: A front black / back white, B front white / back black, update #1, then
  // (#1 displayed) front A := white and readout #2 with the same LUT kind.
  // param: 0x22 value (0xDC OTP partial reloads the OTP LUT, 0xCC custom 2_1 doesn't),
  // bit 8: ping-pong on
  void tPingPong(int param) {
    const uint8_t upd = param & 0xFF;
    prepare(Layout().box(A, "fr0 bk1").box(B, "fr1 bk0"));
    displayOption(param & 0x100);
    if (upd == 0xCC)
      loadLut(SSD1681_LIGHTMYINK_CUSTOM_2_1);
    fill(0x24, ram(A), kBlack);
    fill(0x26, ram(B), kBlack);
    refresh(upd, false, "#1 probe");
    fill(0x24, ram(A), kWhite);
    refresh(upd, false, "#2 readout");
  }

  // As T58-T60 with ping-pong off: if the partials no longer swap, the full refresh
  // gives the same result after 1 and 2 of them. param: number of custom partials
  void tPingPongFull(int partials) {
    prepare(Layout().box(A, "fr0"));
    displayOption(false);
    loadLut(SSD1681_LIGHTMYINK_CUSTOM_2_1);
    for (int i = 0; i < partials; i++)
      refresh(0xCC, false, "partial, no change");
    fill(0x24, ram(A), kBlack);
    refresh(0xD4, false, "full");
  }

  // --- Refreshing a band of rows -------------------------------------------------
  // 0x01 MUX = rows - 1 and 0x0F gate scan start limit the refresh to a band of gate
  // lines (columns can't be limited, T10). Is it faster, what happens to the rows
  // that aren't scanned, and to the pixels beside the square inside the band?
  // The square: RAM rows 88-119, x 80-119, the gap between the box rows. Boxes A, C, E
  // black and B, D, F white show fading on both colours.
  // param: toggles of the square (odd ends black) | 0x100 band | 0x200 custom 2_1 LUT
  void tBandRefresh(int param) {
    const int toggles = param & 0xFF;
    const bool band = param & 0x100, custom = param & 0x200;
    prepare(Layout().box(A, "blk").box(B, "wht").box(C, "blk").box(D, "wht").box(E, "blk").box(F, "wht"));
    for (int box : {A, C, E}) {
      fill(0x24, ram(box), kBlack);
      fill(0x26, ram(box), kBlack);
    }
    refresh(0xD4, false, "glass A C E black");
    if (custom)
      loadLut(SSD1681_LIGHTMYINK_CUSTOM_2_1);
    if (band) {
      cmd(0x01, {31, 0x00, 0x00}); // 32 gate lines
      cmd(0x0F, {88, 0x00});       // starting at gate 88
    }
    const Box square {80, 88, 40, 32}; // RAM coordinates
    for (int i = 0; i < toggles; i++) {
      // Front only: outside the square both buffers already hold the same picture
      setBox(square);
      begin(0x24);
      stream(square.w / 8 * square.h, i % 2 ? kWhite : kBlack, square.w / 8);
      end();
      refresh(custom ? 0xCC : 0xDC, false, band ? "band" : "whole screen");
    }
  }

  // --- Panel geometry ---------------------------------------------------------

  void tGeometry(int param) {
    prepare(Layout().box(A, "fr0").box(F, "fr0"));
    fill(0x24, ram(A), kBlack);
    fill(0x24, ram(F), kBlack);
    if (param == 0)
      cmd(0x01, {99, 0x00, 0x00});   // 100 gate lines
    else if (param == 1)
      cmd(0x01, {0xC7, 0x00, 0x01}); // TB: gate scan direction
    else if (param == 3)
      cmd(0x01, {0xC7, 0x00, 0x04}); // GD: first gate output G1 instead of G0
    else if (param == 4)
      cmd(0x01, {0xC7, 0x00, 0x02}); // SM: interlaced gate scan (left/right halves)
    else
      cmd(0x0F, {50, 0x00});         // Gate scan start? (SSD1675 command)
    refresh(0xD4);
  }

  // --- Which bank does a full refresh show? -------------------------------------
  // T58-T68 left it open, and each of them started from whatever the menu or the
  // previous test left. Here one wake runs many full refreshes back to back, each
  // after a few operations, with a big F in the front buffer and a big B in the back,
  // so only the change between consecutive steps matters.
  // T76 tested "a full always shows the same physical bank" (expect = that
  // prediction): wrong. Its answer gave the rule T77 checks (expect = rule): a full
  // shows the front iff an even number of mode 2 updates ran since the previous full.
  enum class ParOp : uint8_t { None, Dc, D8, D0, C0, Lut, Cc, PpOff, PpOn, SleepReset, HwReset, SwReset };
  struct ParStep { ParOp ops[3]; const char* what; char expect; };
  struct ParTable { const ParStep* steps; int n; };

  constexpr ParStep kParFixedBank[] = {
    {{}, "full only (reference)", 'F'},
    {{}, "full only", 'B'},
    {{}, "full only", 'F'},
    {{ParOp::Dc}, "1x DC partial", 'F'},
    {{ParOp::Dc, ParOp::Dc}, "2x DC partial", 'B'},
    {{ParOp::D8}, "D8 (mode 2, no display)", 'B'},
    {{ParOp::D0}, "D0 (OTP LUT load only)", 'F'},
    {{ParOp::C0}, "C0 (analog on only)", 'B'},
    {{ParOp::Lut}, "0x32 custom LUT load", 'F'},
    {{ParOp::Lut, ParOp::Cc}, "LUT + 1x CC partial", 'F'},
    {{ParOp::Lut, ParOp::Cc, ParOp::Cc}, "LUT + 2x CC partial", 'B'},
    {{ParOp::PpOff}, "0x37 ping-pong off", 'F'},
    {{ParOp::Lut, ParOp::Cc, ParOp::Cc}, "LUT + 2x CC, pp off (T68)", 'B'},
    {{ParOp::PpOn}, "0x37 ping-pong on", 'F'},
    {{ParOp::SleepReset}, "0x10 01 + HW reset", 'B'},
    {{ParOp::SleepReset}, "0x10 01 + HW reset", 'F'},
    {{ParOp::HwReset}, "HW reset (no sleep)", 'B'},
    {{ParOp::SwReset}, "SWRESET + init", 'F'},
    {{ParOp::SwReset}, "SWRESET + init", 'B'},
    {{}, "full only", 'F'},
  };

  // '?': the rule doesn't say (resets, 0x37), F/B: what it predicts
  constexpr ParStep kParRule[] = {
    {{}, "full only (reference)", 'F'},
    {{ParOp::Dc}, "1x DC", 'B'},
    {{ParOp::Dc, ParOp::Dc, ParOp::Dc}, "3x DC", 'B'},
    {{ParOp::Dc, ParOp::Lut, ParOp::Cc}, "DC + LUT + CC", 'F'},
    {{ParOp::Cc, ParOp::D0}, "CC + D0", 'B'},
    {{ParOp::Dc, ParOp::C0}, "DC + C0", 'B'},
    {{ParOp::D8, ParOp::D8}, "2x D8", 'F'},
    {{ParOp::Dc, ParOp::SleepReset}, "DC + sleep + HW reset", 'B'},
    {{ParOp::SleepReset, ParOp::Dc}, "sleep + HW reset + DC", 'B'},
    {{ParOp::Dc, ParOp::HwReset}, "DC + HW reset", 'B'},
    {{ParOp::Dc, ParOp::SwReset}, "DC + SWRESET (B: kept)", '?'},
    {{ParOp::SwReset, ParOp::Dc}, "SWRESET + DC", 'B'},
    {{}, "full only", 'F'},
    {{ParOp::PpOn}, "0x37 ping-pong on", '?'},
    {{}, "full only", '?'},
    {{ParOp::SwReset}, "SWRESET", 'F'},
    {{ParOp::PpOff}, "0x37 ping-pong off", '?'},
    {{ParOp::SleepReset}, "sleep + HW reset", '?'},
    {{ParOp::SwReset}, "SWRESET", 'F'},
    {{}, "full only", 'F'},
  };

  // T78/T79: the driver's sequences, 0x22 only written when it changes, so most full
  // refreshes are a bare 0x20 (the wake stub never writes 0x22). Expect = T77 rule
  constexpr ParStep kParDriver[] = {
    {{}, "full (reference)", 'F'},
    {{}, "full, bare 0x20", 'F'},
    {{}, "full, bare 0x20", 'F'},
    {{ParOp::Dc}, "DC, full", 'B'},
    {{}, "full, bare 0x20", 'F'},
    {{ParOp::Dc, ParOp::Dc}, "2x DC, full", 'F'},
    {{}, "full, bare 0x20", 'F'},
    {{ParOp::Dc, ParOp::SleepReset}, "DC, sleep + HW reset, full", 'B'},
    {{ParOp::SleepReset}, "sleep + HW reset, bare 0x20", 'F'},
    {{}, "full, bare 0x20", 'F'},
    {{ParOp::Dc}, "DC, full", 'B'},
    {{}, "full, bare 0x20", 'F'},
  };

  constexpr ParTable kParTables[] = {
    {kParFixedBank, int(std::size(kParFixedBank))},
    {kParRule, int(std::size(kParRule))},
    {kParDriver, int(std::size(kParDriver))},
  };

  void parOp(ParOp op) {
    switch (op) {
      case ParOp::None: break;
      case ParOp::Dc: refresh(0xDC, false, "partial"); break;
      case ParOp::D8: refresh(0xD8, false, "no display"); break;
      case ParOp::D0: refresh(0xD0, false, "LUT load only"); break;
      case ParOp::C0: refresh(0xC0, false, "analog on only"); break;
      case ParOp::Lut: loadLut(SSD1681_LIGHTMYINK_CUSTOM_2_1); break;
      case ParOp::Cc: refresh(0xCC, false, "custom partial"); break;
      case ParOp::PpOff: displayOption(false); break;
      case ParOp::PpOn: displayOption(true); break;
      case ParOp::SleepReset: sleepAndReset(); break;
      case ParOp::HwReset: hwReset(); break;
      case ParOp::SwReset: swReset(); initRegs(); break;
    }
  }

  void drawBig(int step, int n, const char* what, char letter, const char* footer) {
    auto& c = *sCanvas;
    c.setRotation(rot());
    c.fillScreen(1);
    c.setTextColor(0);
    c.setTextWrap(false);
    c.setTextSize(1);
    c.setCursor(2, 2);
    c.printf("T%02d step %02d/%02d", sTestNum, step + 1, n);
    c.setCursor(2, 12);
    c.print(what);
    c.setCursor(2, 190);
    c.print(footer);
    c.setTextSize(16); // 96x128
    c.setCursor(52, 40);
    c.print(letter);
    c.setTextSize(1);
  }

  void drawParPicture(const ParTable& t, int step, char letter) {
    drawBig(step, t.n, t.steps[step].what, letter,
            letter == 'F' ? "front buffer (0x24)" : "back buffer (0x26)");
  }

  // Canvas to RAM, bytes inverted when 0x21 inverts the RAM (the watch runs 0x88)
  void writeCanvasInv(uint8_t ramCmd, bool inv) {
    if (inv)
      for (auto* b = sCanvas->getBuffer(), *e = b + kW / 8 * kH; b < e; b++) *b ^= 0xFF;
    writeCanvas(ramCmd);
  }

  // param: table index | 0x10 driver style (0x22 only on change) | 0x20 0x21 = 0x88
  void tFullParity(int param) {
    const auto& t = kParTables[param & 0xF];
    const bool inv = param & 0x20;
    prepare(Layout().noSwitch());
    sDriverStyle = param & 0x10;
    if (inv)
      cmd(0x21, {0x88}); // Both buffers inverted, as the driver with mInvert off
    for (int i = 0; i < t.n; i++) {
      const auto& s = t.steps[i];
      ESP_LOGE(TAG, "--- step %02d: %s (expect %c)", i + 1, s.what, s.expect);
      for (auto op : s.ops)
        parOp(op);
      drawParPicture(t, i, 'F');
      writeCanvasInv(0x24, inv);
      drawParPicture(t, i, 'B');
      writeCanvasInv(0x26, inv);
      refresh(0xD4, false, "full");
      delay(4000);
    }

    // Summary of the expected letters. Same picture in both buffers, so the full
    // refresh shows it whichever bank it picks
    sDriverStyle = false;
    cmd(0x21, {0x00});
    auto& c = *sCanvas;
    c.setRotation(rot());
    c.fillScreen(1);
    c.setTextColor(0);
    c.setTextSize(1);
    c.setCursor(2, 2);
    c.printf("T%02d expected:", sTestNum);
    for (int i = 0; i < t.n; i++) {
      c.setCursor(2, 12 + 9 * i);
      c.printf("%02d %c %s", i + 1, t.steps[i].expect, t.steps[i].what);
    }
    writeCanvas(0x24);
    writeCanvas(0x26);
    refresh(0xD4, false, "summary");
  }

  // --- What a full refresh leaves in the RAM -------------------------------------
  // The first single-bank FULL driver left the old picture ANDed with the new one
  // on the first partial after a full refresh. Two models fit T76-T79:
  //  A: a full shows the front buffer and swaps the banks like a partial, so the
  //     back buffer then holds the glass (what the driver assumed);
  //  Z: a full always shows one physical bank and points 0x24 at it, no swap, so the
  //     front buffer holds the glass and the back buffer is stale.
  // Pictures: F = front, B = back, N = a new frame written to the front only.
  // param 1 (T81): analog off (0x22 03, no swap T57) during the holds, as the driver
  // hibernates right after BUSY: T78-T80 faded while the analog stayed on
  void tFullLeaves(int param) {
    constexpr int n = 5;
    prepare(Layout().noSwitch());
    auto fullFB = [&](int step, const char* what) {
      drawBig(step, n, what, 'F', "front buffer (0x24)");
      writeCanvas(0x24);
      drawBig(step, n, what, 'B', "back buffer (0x26)");
      writeCanvas(0x26);
      refresh(0xD4, false, "full");
    };
    auto newFrame = [&](int step, const char* what) {
      drawBig(step, n, what, 'N', "new frame, front only");
      writeCanvas(0x24);
    };
    auto hold = [&] {
      if (param)
        refresh(0x03, false, "analog off");
      delay(5000);
    };

    ESP_LOGE(TAG, "--- step 01: F front, B back, full (both models: F)");
    fullFB(0, "full: F front, B back");
    hold();
    ESP_LOGE(TAG, "--- step 02: no writes, DC (A: B, Z: F stays)");
    refresh(0xDC, false, "partial, no writes");
    hold();
    ESP_LOGE(TAG, "--- step 03: SWRESET, F front, B back, full (both: F)");
    swReset(); initRegs();
    fullFB(2, "SWRESET, full again");
    hold();
    ESP_LOGE(TAG, "--- step 04: D8, N to front, DC (Z: clean N, A: N + leftovers)");
    refresh(0xD8, false, "no display");
    newFrame(3, "D8, N front only, partial");
    refresh(0xDC, false, "partial");
    hold();
    ESP_LOGE(TAG, "--- step 05: SWRESET, F/B full, N to front, DC (Z: N + leftovers, A: clean N)");
    swReset(); initRegs();
    fullFB(4, "full, then N front, partial");
    hold();
    newFrame(4, "full, then N front, partial");
    refresh(0xDC, false, "partial");
  }

  // --- Timing -----------------------------------------------------------------

  void tTiming(int) {
    prepare(Layout().box(A, "flips"));
    const auto a = ram(A);
    bool black = false;
    auto flip = [&] { black = !black; fill(0x24, a, black ? kBlack : kWhite); };
    flip(); refresh(0xD4, false, "full OTP");
    flip(); refresh(0xDC, false, "partial OTP");
    flip(); refresh(0xF7, false, "full +temp +off");
    flip(); refresh(0xFF, false, "partial +temp +off");
    flip(); loadLut(SSD1681_LIGHTMYINK_CUSTOM_2_1); refresh(0xCC, false, "custom 2_1");
    flip(); loadLut(SSD1681_LIGHTMYINK_CUSTOM_6_1); refresh(0xCC, false, "custom 6_1");
    refresh(0xC0, false, "analog on only");
    refresh(0x03, false, "analog+clk off");
    refresh(0xC0, false, "analog on again");
    // Show the numbers under box A (the canvas still holds the base picture)
    auto& c = *sCanvas;
    for (int i = 0; i < sNumTimings; i++) {
      c.setCursor(2, 86 + 8 * i);
      c.printf("%02X %5dms %s", sTimings[i].upd, sTimings[i].ms, sTimings[i].what);
    }
    writeCanvas(0x24);
    refresh(0xDC, false, "show results");
  }

  constexpr Test kTests[] = {
    {"full shows front?", "mode 1: which buffer shows", tFullShowsFront, 0,
     {"front: A blk  B wht  C hstripe", "back : A wht  B blk  C vstripe", "mix: C grid / other"}},
    {"partial drives diff?", "mode 2: back!=front px only", tPartialDiff, 0,
     {"diff: A blk B wht C wht D hstr", "all : A blk B blk C wht D hstr", "back: A wht B blk C blk D wht"}},
    {"partial w/o 0xD8 switch", "partial right after full", tPartialDiff, 1,
     {"same as T02 diff: no switch", "  needed after a full refresh", "other: switch needed (see T02)"}},

    {"copy: partial 0xDC", "RAM after a mode 2 update", tCopyProbe, 0xDC,
     {"copy back<-front: A wht B wht", "swap           : A wht B blk", "no change      : A blk B wht"}},
    {"copy: full 0xD4", "RAM after a mode 1 update", tCopyProbe, 0xD4,
     {"copy back<-front: A wht B wht", "swap           : A wht B blk", "no change      : A blk B wht"}},
    {"copy: no display 0xD8", "RAM after update w/o display", tCopyProbe, 0xD8,
     {"copy back<-front: A wht B wht", "swap           : A wht B blk", "no change      : A blk B wht"}},
    {"copy: full 0xF7", "mode 1 +temp +analog off", tCopyProbe, 0xF7,
     {"copy back<-front: A wht B wht", "swap           : A wht B blk", "no change      : A blk B wht"}},
    {"copy: partial 0xFF", "mode 2 +temp +analog off", tCopyProbe, 0xFF,
     {"copy back<-front: A wht B wht", "swap           : A wht B blk", "no change      : A blk B wht"}},
    {"copy: custom LUT 0xCC", "mode 2 with custom 2_1 LUT", tCopyProbe, 0x100 | 0xCC,
     {"copy back<-front: A wht B wht", "swap           : A wht B blk", "no change      : A blk B wht"}},

    {"refresh in window?", "update limited to RAM window", tRefreshWindow, 0,
     {"window only : A blk  B wht", "whole screen: A blk  B blk"}},
    {"copy in window?", "copy limited to RAM window", tCopyWindow, 0,
     {"whole copy : A wht  B wht", "window copy: A wht  B blk", "no copy    : A blk  B blk", "(valid if T10 = whole)"}},

    {"counters per RAM?", "0x24/0x26 share X/Y counter", tCounterShared, 0,
     {"white band at mark:", "S: separate counter per RAM", "N: shared, keeps going", "C: 0x26 restarts at window"}},
    {"counter after 0x20", "kept after a refresh?", tCounterAfterRefresh, 0,
     {"K: W blk, stripes at K = kept", "W: stripes at W = reset to", "   window start", "else: stripes elsewhere"}},
    {"counter after sleep", "0x10 + HW reset keeps X/Y?", tCounterAfterReset, 0,
     {"K: W blk, stripes at K = kept", "W: stripes at W, window kept", "P: W blk, stripes at RAM 0,0", "   (screen edge) = POR"}},
    {"counter after 0x12", "SWRESET (+init) keeps X/Y?", tCounterAfterReset, 1,
     {"K/W: stripes at mark (window", "  kept), W blk if RAM kept", "P: stripes at the screen edge", "  (RAM 0,0) = window/counter POR"}},

    {"RAM kept: sleep 1", "0x10 01 + HW reset", tRamKept, 0x01,
     {"kept: A blk", "lost: A wht or noise"}},
    {"RAM kept: sleep 2", "0x10 03 + HW reset", tRamKept, 0x03,
     {"kept: A blk", "lost: A wht or noise"}},
    {"RAM kept: SWRESET", "0x12 + init registers", tRamKept, 0x12,
     {"kept: A blk, text crisp", "cleared white: A wht, text", "  fades", "else: grey / noise, describe"}},

    {"0x21 front inverse", "control, no reset", tFrontInverse, 0,
     {"works : whole screen inverts", "no change: ignored in mode 2"}},
    {"0x21 kept: sleep", "0x10 01 + HW reset", tFrontInverse, 1,
     {"kept : screen inverts", "reset: no change", "(read with T19)"}},
    {"0x22 kept: sleep", "0x20 only after reset", tUpd2Kept, 0,
     {"kept 0xD4: A blk, full flash", "POR 0xFF partial: A wht", "nothing at all: 0x22 = 0?"}},
    {"0x3C kept: sleep", "border black before reset", tBorderKept, 0,
     {"kept : border black", "reset: border not black", "(read with T37 = 0x02)"}},
    {"custom LUT works", "control, no reset", tLutKept, 0,
     {"works : A blk", "else: A wht / noise"}},
    {"custom LUT kept: sleep", "0x10 01 + HW reset", tLutKept, 1,
     {"kept : A blk", "lost : A wht / noise", "(read with T23)"}},
    {"Y first 0x11=07", "control, no reset", tEntryYFirst, 0,
     {"A: bar at mark Y = Y first", "A: bar at mark X = ignored", "B: bar at bottom/top row"}},
    {"0x11 kept: sleep", "0x10 01 + HW reset", tEntryYFirst, 1,
     {"kept : A bar at mark Y", "reset: A bar at mark X", "(read with T25)"}},

    {"entry modes X+-Y+-", "where does writing start", tEntryModes, 0,
     {"block = start corner, rot 2:", "A TL B TR C BL D BR", "E,F: same stripe at right", "  edge = bits not mirrored"}},
    {"wrap at window end", "write 1.25x the window", tWrap, 0,
     {"W: stripes at W = wraps", "above A (over label) = runs", "  past the window", "no stripes: stops at end"}},
    {"0x45 with 3 params", "4th param kept or zeroed?", tWrap, 1,
     {"W: stripes at W = 4th reset", "above A: old 4th (1) kept", "(read with T28)"}},
    {"counter outside win", "counter in B, window A", tCounterOutside, 0,
     {"B blk: counter used as is", "A blk: snapped to window", "nothing: write ignored", "else: describe"}},
    {"window X end<start", "X+ mode, X start > X end", tWindowReversed, 0,
     {"describe where black lands", "A/B/C, rows, full width..."}},

    {"0x21 back inverse", "mode 2, back raw white in A", tBackInverse, 0,
     {"works  : A wht", "ignored: A blk"}},
    {"0x21 front bypass 0", "mode 2, no RAM change", tFrontBypass, 0,
     {"as 0   : screen goes black", "ignored: no change"}},
    {"0x21 back bypass 0", "mode 2, front black in A", tBackBypass, 0,
     {"works  : A wht", "ignored: A blk"}},

    {"border 0x00", "GS transition, LUT0", tBorder, 0x00, {"look at the border colour"}},
    {"border 0x01", "GS transition, LUT1", tBorder, 0x01, {"look at the border colour"}},
    {"border 0x02", "GS transition, LUT2 (dark)", tBorder, 0x02, {"look at the border colour"}},
    {"border 0x03", "GS transition, LUT3", tBorder, 0x03, {"look at the border colour"}},
    {"border 0x05", "follow LUT1 (driver white)", tBorder, 0x05, {"look at the border colour"}},
    {"border 0x40", "fix level VSS", tBorder, 0x40, {"look at the border colour"}},
    {"border 0x50", "fix level VSH1", tBorder, 0x50, {"look at the border colour"}},
    {"border 0x60", "fix level VSL", tBorder, 0x60, {"look at the border colour"}},
    {"border 0x80", "VCOM", tBorder, 0x80, {"look at the border colour"}},
    {"border 0xC0", "HiZ (POR)", tBorder, 0xC0, {"look at the border colour"}},

    {"0x01 MUX 100 lines", "panel height = 100 gates", tGeometry, 0,
     {"describe: half screen, stretch,", "A/F boxes shown or not"}},
    {"0x01 TB scan bit", "gate scan direction", tGeometry, 1,
     {"mirrored: header at bottom,", "  text upside down", "same: bit ignored"}},
    {"0x0F gate start 50", "scan start (undocumented?)", tGeometry, 2,
     {"shifted 50 lines (wraps)", "same: no such command"}},

    {"timing", "busy ms per 0x22 sequence", tTiming, 0,
     {"times shown under A", "and in the UART log"}},

    // --- Follow-ups after the first run ---
    {"counters follow swap?", "back counter moved, then 0x20", tCounterFollowsSwap, 0,
     {"black band at mark:", "R: counters reset by 0x20", "F: counters swap with the", "   RAM banks"}},
    {"RAM kept: sleep 2, 60s", "0x10 03, wait 1 min, reset", tRamKept, 0x03 | (60 << 8),
     {"wait ~1 min for the readout", "kept: A blk", "lost: A wht or noise"}},
    {"sleep current 1 vs 2", "PPK2: 20 s mode 1, 20 s mode 2", tSleepCurrent, 0,
     {"measure the current in both", "  20 s phases (UART says when)", "then A blk = RAM survived both"}},
    {"X- bit order", "1 px line, first byte per row", tBitOrder, 0,
     {"F: line at its right edge", "E line at its left edge:", "  X- mirrors bits too", "E line 7 px in: not mirrored"}},
    {"same colour OTP x1", "BB/WW drive? 1 partial", tSameColour, 1,
     {"w/k = glass wht/blk, BB/WW =", "  both buffers blk/wht", "A wht B blk: no-op, grey: drive", "C blk D wht: controls"}},
    {"same colour OTP x5", "BB/WW drive? 5 partials", tSameColour, 5,
     {"w/k = glass wht/blk, BB/WW =", "  both buffers blk/wht", "A, B darker/lighter than T53?", "C blk D wht: controls"}},
    {"same colour LUT 2_1", "custom LUT, 1 update", tSameColour, 0x100 | 1,
     {"w/k = glass wht/blk, BB/WW =", "  both buffers blk/wht", "A wht B blk: no-op, grey: drive", "C blk D wht: controls"}},
    {"swap: analog on 0xC0", "no LUT, no display", tCopyProbe, 0xC0,
     {"copy back<-front: A wht B wht", "swap           : A wht B blk", "no change      : A blk B wht"}},
    {"swap: off 0x03", "analog/clock off only", tCopyProbe, 0x03,
     {"copy back<-front: A wht B wht", "swap           : A wht B blk", "no change      : A blk B wht"}},

    {"full after 0 partials", "control: as T01", tFullAfterPartials, 0,
     {"updated: A blk", "not updated: A wht", "log: full busy ~1912 ms"}},
    {"full after 1 partial", "odd number of swaps", tFullAfterPartials, 1,
     {"updated: A blk", "not updated: A wht", "log: full busy ~1912 ms"}},
    {"full after 2 partials", "even number of swaps", tFullAfterPartials, 2,
     {"updated: A blk", "not updated: A wht", "log: full busy ~1912 ms"}},
    {"1 partial, D0, full", "old driver: extra update", tFullAfterPartials, 0x20 | 1,
     {"updated: A blk", "not updated: A wht", "log: full busy ~1912 ms"}},
    {"1 custom partial, full", "custom 2_1 LUT, then OTP full", tFullAfterPartials, 0x10 | 1,
     {"updated: A blk", "not updated: A wht", "log: full busy ~1912 ms"}},
    {"1 partial, both buffers", "fix? frame in 0x24 and 0x26", tFullAfterPartials, 0x40 | 1,
     {"updated: A blk", "not updated: A wht", "log: full busy ~1912 ms"}},

    {"ping-pong off, DC", "0x37 F=00, OTP partial", tPingPong, 0xDC,
     {"copy back<-front: A wht B wht", "swap           : A wht B blk", "no change      : A blk B wht"}},
    {"ping-pong off, CC", "0x37 F=00, custom LUT", tPingPong, 0xCC,
     {"copy back<-front: A wht B wht", "swap           : A wht B blk", "no change      : A blk B wht"}},
    {"ping-pong on, CC", "0x37 F=40, custom LUT", tPingPong, 0x100 | 0xCC,
     {"copy back<-front: A wht B wht", "swap           : A wht B blk", "no change      : A blk B wht"}},
    {"pp off: 1 partial, full", "custom partial, then full", tPingPongFull, 1,
     {"updated: A blk", "not updated: A wht", "compare with T68"}},
    {"pp off: 2 partials, full", "custom partials, then full", tPingPongFull, 2,
     {"updated: A blk", "not updated: A wht", "same as T67: no more swaps"}},
    {"0x01 GD bit", "first gate G1 instead of G0", tGeometry, 3,
     {"describe: shifted 1 line,", "  rows swapped in pairs, same"}},
    {"0x01 SM bit", "interlaced gate scan", tGeometry, 4,
     {"describe: rows interleaved,", "  halves swapped, same"}},

    {"square, whole, OTP x5", "baseline: all 200 rows", tBandRefresh, 5,
     {"centre square ends black", "log: busy per refresh", "  (baseline for T72)"}},
    {"square, band, OTP x5", "only rows 88-119 scanned", tBandRefresh, 0x100 | 5,
     {"square black, in place?", "boxes A-F: unchanged/grey?", "log: busy vs T71"}},
    {"square, band, OTP x21", "degradation outside band", tBandRefresh, 0x100 | 21,
     {"boxes A-F faded/grey?", "row band left/right of", "  square: grey? edges?"}},
    {"square, whole, 2_1 x5", "custom LUT baseline", tBandRefresh, 0x200 | 5,
     {"centre square ends black", "log: busy per refresh", "  (baseline for T75)"}},
    {"square, band, 2_1 x21", "custom LUT, band, 21x", tBandRefresh, 0x300 | 21,
     {"boxes A-F faded/grey?", "band beside square: grey?", "log: busy vs T74"}},

    {"full refresh parity", "20 fulls, F/B after each op", tFullParity, 0,
     {"write down the letter of", "  every step (film it), then", "  compare with the summary"}},
    {"full parity: rule", "F iff even mode 2 since full?", tFullParity, 1,
     {"write down the letter of", "  every step (film it), then", "  compare with the summary"}},
    {"full parity: driver", "0x22 only on change, bare 0x20", tFullParity, 0x10 | 2,
     {"write down the letter of", "  every step (film it), then", "  compare with the summary"}},
    {"full parity: driver inv", "as T78 with 0x21 = 0x88", tFullParity, 0x30 | 2,
     {"write down the letter of", "  every step (film it), then", "  compare with the summary"}},
    {"what a full leaves", "swap after full, or not?", tFullLeaves, 0,
     {"each step: letter seen, and", "  clean or mixed with another", "  letter (leftovers)"}},
    {"what a full leaves, off", "T80, analog off in holds", tFullLeaves, 1,
     {"each step: letter seen, and", "  clean or mixed with another", "  letter (leftovers)"}},
    {"swap: 0x08 after full", "mode 2 bit only, no clock", tSwapProbe, 0x08,
     {"swap   : A wht B blk", "no swap: A blk B wht", "copy   : A wht B wht", "busy ms shown above"}},
    {"swap: 0x88 after full", "mode 2 + clock, no analog", tSwapProbe, 0x88,
     {"swap   : A wht B blk", "no swap: A blk B wht", "copy   : A wht B wht", "busy ms shown above"}},
    {"swap: 0xC8 after full", "+ analog, no LUT load", tSwapProbe, 0xC8,
     {"swap   : A wht B blk", "no swap: A blk B wht", "copy   : A wht B wht", "busy ms shown above"}},
    {"swap: 0xD8 after full", "the driver's (control)", tSwapProbe, 0xD8,
     {"swap   : A wht B blk", "no swap: A blk B wht", "copy   : A wht B wht", "busy ms shown above"}},
    {"swap: 0x98 after full", "clock + LUT load, no analog", tSwapProbe, 0x98,
     {"swap   : A wht B blk", "no swap: A blk B wht", "copy   : A wht B wht", "busy ms shown above"}},
  };
  constexpr int kNumTests = std::size(kTests);

  // -------------------------------------------------------------------------
  // Menu / flow

  void runTest(int i) {
    sTest = &kTests[i];
    sTestNum = i + 1;
    ESP_LOGE(TAG, "=== T%02d %s: %s", sTestNum, sTest->name, sTest->question);
    for (auto l : sTest->legend)
      if (l) ESP_LOGE(TAG, "    %s", l);
    sState.menuOnGlass = false;
    sTest->fn(sTest->param);
    ESP_LOGE(TAG, "=== T%02d done", sTestNum);
    sState.inResult = true;
  }

  // fast: only valid right after another menu draw. The HW reset keeps registers and
  // RAM (T14, T16, T20-T26) and after a refresh the back buffer holds the glass (swap,
  // T04), so writing the new menu to the front buffer and one partial refresh is enough
  void showMenu(bool fast = false) {
    static constexpr Test kMenu {"display tests", "UP/DN select, MENU run", nullptr, 0,
      {"on a result: MENU again,", "UP/DN run prev/next, BACK menu"}};
    sTest = &kMenu;
    sTestNum = 0;
    drawBase(Layout());
    auto& c = *sCanvas;
    const int first = std::clamp(sState.sel - 7, 0, std::max(0, kNumTests - 15));
    for (int i = first; i < std::min(first + 15, kNumTests); i++) {
      c.setCursor(2, 26 + 9 * (i - first));
      c.printf("%c%02d %s", i == sState.sel ? '>' : ' ', i + 1, kTests[i].name);
    }
    if (fast && sState.menuOnGlass) {
      hwReset();
      writeCanvas(0x24);
      refresh(0xDC, false, "menu fast");
    } else {
      hwReset();
      swReset();
      initRegs();
      writeCanvas(0x24);
      writeCanvas(0x26);
      refresh(0xD4, false, "menu");
    }
    sState.menuOnGlass = true;
    sState.inResult = false;
  }

  void handle(Touch::Btn btn) {
    auto& s = sState.sel;
    const auto step = [&](int d) { s = (s + d + kNumTests) % kNumTests; };
    if (sState.inResult) {
      switch (btn) {
        case Touch::MENU: runTest(s); break;
        case Touch::DOWN: step(1); runTest(s); break;
        case Touch::UP: step(-1); runTest(s); break;
        case Touch::BACK: showMenu(); break;
        default: break;
      }
    } else {
      switch (btn) {
        case Touch::MENU: runTest(s); break;
        case Touch::DOWN: step(1); showMenu(true); break;
        case Touch::UP: step(-1); showMenu(true); break;
        case Touch::BACK: showMenu(); break; // Full redraw: clears ghosting
        default: break;
      }
    }
  }
}

void DisplayTest::runImpl() {
  const bool firstBoot = wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED;
  Touch touch{kSettings.mTouch};

  // No auto light sleep while awake: it suspends the UARTs (XOFF), the prime suspect
  // for a trace that stopped mid-frame and froze T51. Power doesn't matter here.
  esp_pm_lock_handle_t noSleep;
  if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "dtest", &noSleep) == ESP_OK)
    esp_pm_lock_acquire(noSleep);
  if (firstBoot) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    Light::off();
  }
  touch.setUp(false);

  {
    // Display bring-up, same pins/sequence as Display::Display()
    Spi spi;
    uSpi::init(false);
    pinMode(HW::Display::Cs, OUTPUT);
    pinMode(HW::Display::Dc, OUTPUT);
    pinMode(HW::Display::Res, OUTPUT);
    pinMode(HW::Display::Busy, INPUT);
    digitalWrite(HW::Display::Cs, HIGH);
    digitalWrite(HW::Display::Dc, HIGH);
    if (rtc_gpio_is_valid_gpio((gpio_num_t)HW::Display::Res))
      rtc_gpio_hold_dis((gpio_num_t)HW::Display::Res);
    digitalWrite(HW::Display::Res, HIGH);

    auto power = Power::Lock(Power::Flag::Display);
    GFXcanvas1 canvas(kW, kH);
    sCanvas = &canvas;

    if (firstBoot) {
      const int start = CONFIG_LIGHTINK_DISPLAY_TEST_START;
      if (start > 0 && start <= kNumTests) {
        sState.sel = start - 1;
        runTest(sState.sel);
      } else {
        showMenu();
      }
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TOUCHPAD) {
      handle(Trace::touch(touch.readAndClear()));
    }

    cmd(0x10, {0x01}); // Hibernate, keep RAM
    sCanvas = nullptr;
    if (rtc_gpio_is_valid_gpio((gpio_num_t)HW::Display::Res))
      rtc_gpio_hold_en((gpio_num_t)HW::Display::Res);
  }

  // Sleep until the next touch. Auto light sleep (esp_pm) leaves a short timer
  // wakeup armed, which would bring us straight back: clear every source first
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  touch.enable();
  esp_set_deep_sleep_wake_stub(NULL);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);
  touch.clear();
  Trace::sleep();
  esp_deep_sleep_disable_rom_logging();
  esp_deep_sleep_start();
}

#endif
