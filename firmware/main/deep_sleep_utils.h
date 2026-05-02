#pragma once

#include "soc/io_mux_reg.h"
#include "rom/gpio.h"

#if(HW_VERSION >= 10)
  // Set wakeup timer when we guess display will finish refreshing, to put display to hibernation
  #define GPIO_OUTPUT_SET(gpio_no, bit_value) gpio_output_set(bit_value<<gpio_no, (bit_value ? 0 : 1)<<gpio_no, 1<<gpio_no,0)
  #define GPIO_DIS_OUTPUT(gpio_no)   (gpio_output_set(0,0,0, 1<<gpio_no))
  #define GPIO_INPUT_GET(gpio_no)    ((gpio_input_get()>>gpio_no)&BIT0)
#endif

template<uint8_t i> void gpio_mode_input();
template<uint8_t i> void gpio_mode_output();