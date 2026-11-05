#include "esp_attr.h"
#include "soc/reg_base.h"
#include "soc/gpio_reg.h"
#include "hal/gpio_types.h"

#include "deep_sleep_utils.h"

// Deep Sleep pin controls
#define GPIO_MODE_INPUT(pin) \
  PIN_FUNC_SELECT(GPIO_PIN_REG_##pin, PIN_FUNC_GPIO); \
  PIN_INPUT_ENABLE(GPIO_PIN_REG_##pin); \
  REG_WRITE(GPIO_ENABLE_W1TC_REG, (1UL << pin));

#define GPIO_MODE_OUTPUT(pin) \
  PIN_FUNC_SELECT(GPIO_PIN_REG_##pin, PIN_FUNC_GPIO); \
  PIN_INPUT_DISABLE(GPIO_PIN_REG_##pin); \
  REG_WRITE(GPIO_ENABLE_W1TC_REG, (1UL << pin));
  
// Helper code to allow the upper macros to be called with constexpr arguments from HW
#if CONFIG_IDF_TARGET_ESP32S3
#define GPIO_PIN_LIST X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) \
X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(26) X(27) X(28) X(29) X(30) X(31)
#define GPIO_INPUT_PIN_LIST
#else
#define GPIO_PIN_LIST X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) \
X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27)
#define GPIO_INPUT_PIN_LIST X(35) X(36) X(37) X(38) X(39)
#endif

#define X(x) template<> RTC_SLOW_ATTR void gpio_mode_input<x>() { GPIO_MODE_INPUT(x); }
GPIO_PIN_LIST
#undef X
#define X(x) template<> RTC_SLOW_ATTR void gpio_mode_input<x>() {}
GPIO_INPUT_PIN_LIST
#undef X

#define X(x) template<> RTC_SLOW_ATTR void gpio_mode_output<x>() { GPIO_MODE_OUTPUT(x); }
GPIO_PIN_LIST
#undef X