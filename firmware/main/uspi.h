#pragma once

#include "SPI.h"
#include "soc/reg_base.h"
#include "soc/spi_struct.h"
#include "soc/dport_reg.h"

#include "hardware.h"
#include "settings.h"
#include "deep_sleep_utils.h"

#include "hal/clk_gate_ll.h"

#include "hal/gpio_ll.h"

namespace uSpi {
  void RTC_IRAM_ATTR init(); // Should be called from the 
  void RTC_IRAM_ATTR transfer(const void *data_in, uint32_t len);
  void RTC_IRAM_ATTR transfer(const uint8_t data_in);
  void RTC_IRAM_ATTR command(uint8_t value);

  // Display specific (maybe refactor to uDisplay)
  void RTC_IRAM_ATTR setRamArea(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
  void RTC_IRAM_ATTR writeArea(const uint8_t* ptr, uint8_t x, uint8_t y, uint8_t w, uint8_t h);
  void RTC_IRAM_ATTR refresh();
  void RTC_IRAM_ATTR hibernate();
};
