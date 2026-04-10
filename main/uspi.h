
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
  constexpr auto clockDiv = 266241; // or even 8193 for 26MHz SPI !

  spi_dev_t& dev = *(reinterpret_cast<volatile spi_dev_t *>(DR_REG_SPI3_BASE));

  void RTC_IRAM_ATTR init() {
    // Initialize all to 0
    memset((void*)&dev, 0, sizeof(dev));

    // Set
    dev.clock.val = clockDiv;
    dev.user.usr_mosi = 1;
    dev.user.usr_miso = 0; // We do not want to read anything
    dev.user.doutdin = 1;
    dev.user.cs_setup = 1;
    dev.user.cs_hold = 1;

    // Mode 0
    dev.user.ck_out_edge = 0;
    dev.ctrl.wr_bit_order = 0; //MSBFIRST
    dev.ctrl.rd_bit_order = 0;

    gpio_mode_output<HW::Display::Dc>();
    gpio_mode_output<HW::Spi::Sck>(); // NEEDED FOR HW_V3
    gpio_mode_output<HW::Spi::Mosi>(); // NEEDED FOR HW_V3

#if (HW_VERSION < 10)
    gpio_matrix_out(HW::Spi::Sck, VSPICLK_OUT_IDX, false, false);
    gpio_matrix_out(HW::Spi::Mosi, VSPID_IN_IDX, false, false);
    gpio_matrix_out(HW::Display::Cs, VSPICS0_OUT_IDX, false, false);
    dev.pin.val = dev.pin.val & ~((1 << 0) & SPI_SS_MASK_ALL);
#else
    dev.clk_gate.clk_en = 1;
    dev.clk_gate.mst_clk_sel = 1;
    dev.clk_gate.mst_clk_active = 1;
    dev.dma_conf.rx_afifo_rst = 1;
    dev.dma_conf.buf_afifo_rst = 1;
    dev.dma_conf.tx_seg_trans_clr_en = 1;
    dev.dma_conf.rx_seg_trans_clr_en = 1;

    // These pins need to be set as GPIO before can be used in the matrix
    if constexpr (HW::Spi::Sck == 19 || HW::Spi::Mosi == 19 || HW::Display::Cs == 19)
      gpio_ll_iomux_func_sel(IO_MUX_GPIO19_REG, PIN_FUNC_GPIO);
    if constexpr (HW::Spi::Sck == 20 || HW::Spi::Mosi == 20 || HW::Display::Cs == 20)
      gpio_ll_iomux_func_sel(IO_MUX_GPIO20_REG, PIN_FUNC_GPIO);

    gpio_matrix_out(HW::Spi::Sck, SPI3_CLK_OUT_IDX, false, false);
    gpio_matrix_out(HW::Spi::Mosi, SPI3_D_IN_IDX, false, false);
    gpio_matrix_out(HW::Display::Cs, SPI3_CS0_OUT_IDX, false, false);
    dev.misc.val = dev.misc.val & ~((1 << 0) & SPI_SS_MASK_ALL);

    // Update the HW
    dev.cmd.update = 1;
    while (dev.cmd.update);
#endif
  }

  void RTC_IRAM_ATTR transfer(const void *data_in, uint32_t len) {
    size_t longs = len >> 2;
    if (len & 3) {
        longs++;
    }
    uint32_t *data = (uint32_t *)data_in;
    size_t c_len = 0, c_longs = 0;

    while (len) {
        c_len = (len > 64) ? 64 : len;
        c_longs = (longs > 16) ? 16 : longs;
#if (HW_VERSION < 10)
        dev.mosi_dlen.usr_mosi_dbitlen = (c_len * 8) - 1;
        dev.miso_dlen.usr_miso_dbitlen = 0;
#else
        dev.ms_dlen.ms_data_bitlen = (c_len * 8) - 1;
#endif
        for (size_t i = 0; i < c_longs; i++) {
            dev.data_buf[i] = data[i];
        }
#if (HW_VERSION >= 10)
        dev.cmd.update = 1;
        while (dev.cmd.update);
#endif
        dev.cmd.usr = 1; // Submit command

        data += c_longs;
        longs -= c_longs;
        len -= c_len;
        // Faster to wait here (-10us)
        while (dev.cmd.usr); // Wait till previous commands have finished
    }
  }

  void RTC_IRAM_ATTR transfer(const uint8_t data_in) {
    transfer(&data_in, 1);
  }

  void RTC_IRAM_ATTR command(uint8_t value)
  {
    GPIO_OUTPUT_SET(HW::Display::Dc, 0);
    transfer(value);
    GPIO_OUTPUT_SET(HW::Display::Dc, 1);
  }

  void RTC_IRAM_ATTR setX(uint8_t x) {
    command(0x4e); // X start counter
    transfer(x);
  }
  void RTC_IRAM_ATTR setY(uint8_t y) {
    command(0x4f); // Y start counter
    transfer(y);
    //_transfer(0); // No need to write this, default is 0
  };

  void RTC_IRAM_ATTR setRamArea(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    command(0x44);  // X start & end positions (Byte)
    transfer(x / 8);
    transfer((x + w - 1) / 8);
    command(0x45); // Y start & end positions (Line)
    transfer(y);
    transfer(0);
    transfer((y + h - 1));
    //_transfer(0); // No need to write this, default is 0
    setX(x / 8);
    setY(y);
  };

  void RTC_IRAM_ATTR writeArea(const uint8_t* ptr, uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    setRamArea(x, y, w, h);
    command(0x24);
    transfer(ptr, ((uint16_t)h) * w / 8);
  }

  void RTC_IRAM_ATTR refresh() {
    // Set partial mode ? It should be already set, don't touch it
    // command(0x22);
    // transfer(0b11010100 | 0b00001000);
    // Update
    command(0x20);
  }

  void RTC_IRAM_ATTR hibernate() {
      // Sleep
      command(0x10);
      transfer(1);
  }
};
