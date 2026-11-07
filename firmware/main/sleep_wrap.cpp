// The RTC slow clock calibration belongs to the app (Time::calSet), ESP-IDF must not
// measure or change it. ESP32 only, linked with -Wl,--wrap, see CMakeLists.txt.

#include "esp_attr.h"
#include "esp32/rom/rtc.h"
#include "soc/rtc.h"
#include "soc/rtc_cntl_reg.h"

extern "C" {

uint32_t __real_rtc_clk_cal(soc_clk_freq_calculation_src_t sel, uint32_t cycles);
void __real_esp_clk_slowclk_cal_set(uint32_t cal);
void __real_rtc_sleep_low_init(uint32_t period, bool deepSleep);

// IDF measures the slow clock before every sleep (10 cycles, ~300us).
// Skip it and return the stored calibration.
uint32_t IRAM_ATTR __wrap_rtc_clk_cal(soc_clk_freq_calculation_src_t sel, uint32_t cycles)
{
    if (sel == CLK_CAL_RTC_SLOW)
        return REG_READ(RTC_SLOW_CLK_CAL_REG);
    return __real_rtc_clk_cal(sel, cycles);
}

// IDF stores the nominal value on every boot and its own value before sleeping.
// Only take it on power-on, when there is no calibration yet.
void __wrap_esp_clk_slowclk_cal_set(uint32_t cal)
{
    if (REG_READ(RTC_SLOW_CLK_CAL_REG) == 0)
        __real_esp_clk_slowclk_cal_set(cal);
}

// IDF only uses the short RC_FAST (8 MHz) wait on light sleep, use it on deep sleep as well.
// Was the IDF patch "Minor fix to the RTC/Touch issue" (ce50678229c).
void IRAM_ATTR __wrap_rtc_sleep_low_init(uint32_t period, bool deepSleep)
{
    __real_rtc_sleep_low_init(period, deepSleep);
    REG_SET_FIELD(RTC_CNTL_TIMER1_REG, RTC_CNTL_CK8M_WAIT, RTC_CNTL_CK8M_WAIT_SLP_CYCLES);
}

}
