#include "uspi.h"

namespace uSpi {
  static constexpr auto clockDiv = 266241; // =20MHz
  static constexpr auto clockDivOver = 8193; // =26MHz

  static spi_dev_t& dev = *(reinterpret_cast<volatile spi_dev_t *>(DR_REG_SPI3_BASE));

  void init(bool overdrive) {
    // Initialize all to 0
    memset((void*)&dev, 0, sizeof(dev));

    // Set
    dev.clock.val = overdrive ? clockDivOver : clockDiv;
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

  void write(const void *data_in, uint32_t len) {
    size_t longs = len >> 2;
    if (len & 3) {
      longs++;
    }
    uint32_t *data = (uint32_t *)data_in;
    size_t c_len = 0, c_longs = 0;

    while (len) {
      c_len = (len > 64) ? 64 : len;
      c_longs = (longs > 16) ? 16 : longs;
#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32
      dev.mosi_dlen.usr_mosi_dbitlen = (c_len * 8) - 1;
      dev.miso_dlen.usr_miso_dbitlen = 0;
#else
      dev.ms_dlen.ms_data_bitlen = (c_len * 8) - 1;
#endif
      for (size_t i = 0; i < c_longs; i++) {
        dev.data_buf[i] = data[i];
      }
#if !defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S2)
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

  void write(const uint8_t data_in) {
    write(&data_in, 1);
  }

// More optimized version of write, but is minimal
//   void write(const uint8_t data_in) {
// #if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32
//     dev.mosi_dlen.usr_mosi_dbitlen = 7;
//     dev.miso_dlen.usr_miso_dbitlen = 0;
// #else
//     dev.ms_dlen.ms_data_bitlen = 7;
// #endif
//     dev.data_buf[0] = data_in;
// #if !defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S2)
//     dev.cmd.update = 1;
//     while (dev.cmd.update);
// #endif
//     dev.cmd.usr = 1; // Submit command
//     while (dev.cmd.usr); // Wait till previous commands have finished
//   }

  void command(uint8_t value)
  {
    GPIO_OUTPUT_SET(HW::Display::Dc, 0);
    write(value);
    GPIO_OUTPUT_SET(HW::Display::Dc, 1);
  }

  void setRamArea(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    command(0x44);  // X start & end positions (Byte)
    write(x >> 3);
    write((x + w - 1) >> 3);
    command(0x45); // Y start & end positions (Line)
    write(y);
    write(0);
    write((y + h - 1));
    //write(0); // No need to write this, default is 0
    command(0x4e); // X start counter
    write(x >> 3);
    command(0x4f); // Y start counter
    write(y);
    //write(0); // No need to write this, default is 0
  };

  void writeArea(const uint8_t* ptr, uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    setRamArea(x, y, w, h);
    command(0x24);
    write(ptr, ((uint16_t)h) * w / 8);
  }

  void refresh() {
    // Set partial mode ? It should be already set, don't touch it
    // command(0x22);
    // write(0b11010100 | 0b00001000);
    // Update
    command(0x20);
  }

  void hibernate() {
      // Sleep
      command(0x10);
      write(1);
  }
};
