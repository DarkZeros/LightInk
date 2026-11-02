// based on Demo Example from Good Display, available here: http://www.e-paper-display.com/download_detail/downloadsId=806.html
// Panel: GDEH0154D67 : http://www.e-paper-display.com/products_detail/productId=455.html
// Controller : SSD1681 : http://www.e-paper-display.com/download_detail/downloadsId=825.html
//
// Inspired on GxEPD2 by Author: Jean-Marc Zingg
// Library: https://github.com/ZinggJM/GxEPD2
//
// Fully rewriten for this project

#include "display.h"
#include "power.h"
#include "hardware.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

#include "uspi.h"

// The display will remember the config and RAM between runs
// we can remember them and avoid expensive SPI calls
static RTC_DATA_ATTR struct DisplayState {
  bool initialized : 1 {false};
  bool firstRefreshDone : 1 {false};
  bool darkBorder : 1 {false};
  bool inverted : 1 {false};
  bool postInvert : 1 {false};
  bool flip : 1 {false}; // Done by the controller, see setFlip()
  bool oddPartials : 1 {false}; // Odd number of partial refreshes since the last full / SWRESET
  bool backStale : 1 {false}; // Last refresh was full: front = glass, back stale (T80)
  DisplayMode mode {DisplayMode::FULL};
} kState;

int RTC_IRAM_ATTR getSetDisplayMode() { return kState.mode; };
bool RTC_IRAM_ATTR getHwFlip() { return kState.flip; };

// The RAM banks (docs/display_findings.md T76-T80):
// - A partial (mode 2) refresh drives back -> front and swaps: afterwards the back
//   buffer holds the glass, the front one the frame before.
// - A full (mode 1) refresh shows the front buffer iff an even number of partials ran
//   since the last full refresh or SWRESET (sleep and HW reset keep the count), and
//   doesn't swap: afterwards the front buffer holds the glass, the back one is stale.
// So in FULL mode the front buffer is enough, except after an odd number of partials
// (leaving a partial-mode menu), where RAM writes go to both banks. Before the first
// partial after a full refresh, setRefreshMode() swaps once without display (0x98).
int RTC_IRAM_ATTR getRamWrites() { return kState.mode == FULL && kState.oddPartials ? 2 : 1; }
void RTC_IRAM_ATTR countRefresh(bool full) {
  kState.oddPartials = !full && !kState.oddPartials;
  kState.backStale = full;
}

namespace {
  const SPISettings kSpiSettings{Display::kOverdriveSPI ? 26'666'666 : 20'000'000, MSBFIRST, SPI_MODE0};
}

SemaphoreHandle_t sSem = NULL;
void isr(void* ) {
  BaseType_t woken;
  gpio_intr_disable((gpio_num_t)HW::Display::Busy);
  xSemaphoreGiveFromISR(sSem, &woken);
}

// SPI related
void Display::_startTransfer()
{
  if constexpr (kArduinoSPI)
    SPI.beginTransaction(kSpiSettings);
  gpio_set_level((gpio_num_t)HW::Display::Cs, LOW);
}
void Display::_endTransfer()
{
  gpio_set_level((gpio_num_t)HW::Display::Cs, HIGH);
  if constexpr (kArduinoSPI)
    SPI.endTransaction();
}
void Display::_transfer(uint8_t value)
{
  if constexpr (kArduinoSPI) {
    SPI.write(value);
  } else {
    uSpi::write(value);
  }
}
void Display::_transfer(const uint8_t* value, size_t size)
{
  if constexpr (kArduinoSPI) {
    SPI.writeBytes(value, size);
  } else {
    uSpi::write(value, size);
  }
}
void Display::_transferCommand(uint8_t c)
{
  gpio_set_level((gpio_num_t)HW::Display::Dc, LOW);
  _transfer(c);
  gpio_set_level((gpio_num_t)HW::Display::Dc, HIGH);
}


Display::Display() : Adafruit_GFX(WIDTH, HEIGHT) {
  if constexpr (!kArduinoSPI) {
    uSpi::init(Display::kOverdriveSPI);
  }

  // Set pins
  pinMode(HW::Display::Cs, OUTPUT);
  pinMode(HW::Display::Dc, OUTPUT);
  pinMode(HW::Display::Res, OUTPUT);
  pinMode(HW::Display::Busy, INPUT);

  digitalWrite(HW::Display::Cs, HIGH);
  digitalWrite(HW::Display::Dc, HIGH);
  digitalWrite(HW::Display::Res, HIGH);
  
  // Reset HW / Exit Deep Sleep
  if (rtc_gpio_is_valid_gpio((gpio_num_t)HW::Display::Res))
    Power::hold((gpio_num_t)HW::Display::Res, false);
  gpio_set_level((gpio_num_t)HW::Display::Res, LOW);
  delay(1);
  gpio_set_level((gpio_num_t)HW::Display::Res, HIGH); // THE HAT BOARD NEEDS output high
  //pinMode(HW::Display::Res, INPUT_PULLUP);
  if (rtc_gpio_is_valid_gpio((gpio_num_t)HW::Display::Res))
    Power::hold((gpio_num_t)HW::Display::Res, true);

  // Display requires ISR service for busy pin
  gpio_intr_disable((gpio_num_t)HW::Display::Busy);
  gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
  init();
}

void Display::init() {
  // This only needs to be done once
  if (kState.initialized)
    return;

  _startTransfer();
  _transferCommand(0x12); // SW reset all values to factory defaults
  _endTransfer();
  waitWhileBusy();
  kState.oddPartials = false; // SWRESET restarts the count (T77)
  kState.backStale = false;

  _startTransfer();
  _transferCommand(0x01); // Driver output control
  _transfer(0xC7); // 0x0C7 is already the default
  _transfer(0b0);
  _transfer(0b000); // Gate scanning sequence, default 000
  // The 180 degrees rotation is done with the entry mode instead, see setFlip()

  if constexpr (kReduceBoosterTime) {
    // SSD1675B controller datasheet
    _transferCommand(0x0C); // BOOSTER_SOFT_START_CONTROL
    // Set the driving strength of GDR for all phases to maximun 0b111 -> 0xF
    // Set the minimum off time of GDR to minimum 0x4 (values below sould be same)
    _transfer(0xF4); // Phase1 Default value 0x8B
    _transfer(0xF4); // Phase2 Default value 0x9C
    _transfer(0xF4); // Phase3 Default value 0x96
    _transfer(0x00); // Duration of phases, Default 0xF = 0b00 11 11 (40ms Phase 1/2, 10ms Phase 3)
  }

  if constexpr (kFastUpdateTemp) {
    // Write 50ºC fixed temp (fastest update, but less quality)
    _transferCommand(0x1A);
    _transfer(0x32);
    _transfer(0x00);
  }

  // setRamAdressMode
  _transferCommand(0x11); // set ram entry mode
  _transfer(0b11);        //  0bYX adress mode (+1/-0), Default 0b11

  // Set initial refresh mode, will not change until first refresh
  _setRefreshMode(FULL);
  _endTransfer();

  kState.initialized = true;
}

void Display::_setCustomLut(const DisplayMode& mode) {
  auto& lut = [&] -> const LUT& {
    if (mode == GOOD)
      return SSD1681_LIGHTMYINK_CUSTOM_6_1;
    if (mode == QUICK)
      return SSD1681_LIGHTMYINK_CUSTOM_2_1;
    //if (mode == REPAIR)
    //  return SSD1681_LIGHTMYINK_REPAIR;
    return SSD1681_LIGHTMYINK_CUSTOM_6_1;
    // Other possible LUTS to use
    // auto& lut = SSD1681_WAVESHARE_1IN54_V2_LUT_FULL_REFRESH;
    // auto& lut = SSD1681_WAVESHARE_1IN54_V2_LUT_FAST_REFRESH;
    // auto& lut = SSD1681_WAVESHARE_1IN54_V2_LUT_FAST_REFRESH_KEEP;
  }();

  /* Always write main part of LUT register */
  _transferCommand(0x32);
  _transfer(lut.get().data(), 153);
  // End Option (EOPT)
  if (lut.eopt) {
    _transferCommand(0x3F); // set Lut option End
    _transfer(*lut.eopt);
  }
  /* GATE_DRIVING_VOLTAGE */
  if (lut.vgh) {
    _transferCommand(0x03);
    _transfer(*lut.vgh);
  }
  /* SRC_DRIVING_VOLTAGE */
  if (lut.vsh1_vsh2_vsl) {
    _transferCommand(0x04);
    _transfer(lut.vsh1_vsh2_vsl->data(), 3);
  }
  /* SET_VCOM_REG */
  if (lut.vcom) {
    _transferCommand(0x2c);
    _transfer(*lut.vcom);
  }
}

void Display::_setRamArea(const Rect& rect){
  auto& [x, y, w, h] = rect;
  const auto [xs, xe, ys, ye] = ramWindow(kState.flip, x, y, w, h);
  _transferCommand(0x44);  // X start & end positions (Byte)
  _transfer(xs);
  _transfer(xe);
  _transferCommand(0x45); // Y start & end positions (Line)
  _transfer(ys);
  _transfer(0);
  _transfer(ye);
  //_transfer(0); // No need to write this, default is 0
  _transferCommand(0x4e); // X start counter
  _transfer(xs);
  _transferCommand(0x4f); // Y start counter
  _transfer(ys);
  //_transfer(0); // No need to write this, default is 0
}

// 180 degrees in hardware, the software always draws unrotated (see ramWindow)
void Display::setFlip(bool flip)
{
  if (kState.flip == flip)
    return;
  kState.flip = flip;
  _startTransfer();
  _transferCommand(0x11); // Entry mode, kept across the HW reset on every wake (T26)
  _transfer(flip ? 0b00 : 0b11); // X-/Y- when mirrored, X first
  _endTransfer();
}

void Display::setRefreshMode(DisplayMode mode)
{
  if (kState.mode == mode)
    return;
  kState.mode = mode;

  // Can´t change the refresh mode until first frame is rendered
  if (!kState.firstRefreshDone)
    return;

  // A partial needs the glass in the back buffer, a full refresh leaves it in the
  // front one (T80): swap once without driving the panel. Without the display bit
  // only mode 2 + LUT load swaps (T85 D8 vs T84 C8), and it doesn't need the analog:
  // 0x98 = clock + LUT load + mode 2 takes 1 ms of BUSY, 0xD8 92 ms (T86). It loads
  // the OTP LUT, so the custom LUT is loaded after it, by _setRefreshMode()
  if (mode != FULL && kState.backStale) {
    auto powerLock = Power::Lock(Power::Flag::Display);
    _startTransfer();
    _transferCommand(0x22);
    _transfer(0x98);
    _transferCommand(0x20);
    _endTransfer();
    countRefresh(false);
    waitWhileBusy();
  }

  _startTransfer();
  _setRefreshMode(mode);
  _endTransfer();
}

bool Display::lastRefreshFull() const
{
  return kState.backStale;
}
DisplayMode Display::getRefreshMode() const
{
  return kState.mode;
}

void Display::_setRefreshMode(const DisplayMode& mode)
{
  constexpr auto kTurnOn = 0b11000000; // Enables oscillator & analog
  constexpr auto kLoadTemp = 0b00100000;
  constexpr auto kLoadLut = 0b00010000;
  constexpr auto kPartialMode = 0b00001000;
  constexpr auto kDisplay = 0b00000100;
  // constexpr auto kTurnOff = 0b00000011; // Disables oscillator & analog

  // Default modes FAST/FULL are loaded from the ROM
  bool romLut = mode == DisplayMode::FULL || mode == DisplayMode::FAST;

  // Build default updateCommand
  uint8_t updateCommand = kTurnOn; 
  if (romLut) {
    updateCommand |= kLoadLut;
  } else {
    _setCustomLut(mode);
  }
  if (mode != DisplayMode::FULL) {
    updateCommand |= kPartialMode;
  }
  if (!kFastUpdateTemp) {
    updateCommand |= kLoadTemp;
  }

  // No extra update when switching between FULL and a partial mode: a partial works
  // right after a full one (docs/display_findings.md T03), and an update without the
  // display bit still swaps the buffers (T06), leaving the back buffer one frame behind the glass
  _transferCommand(0x22);
  _transfer(updateCommand | kDisplay);
}

// A partial refresh swaps the front and back buffers (docs/display_findings.md
// T04-T09, T49): after it, the back buffer (0x26) holds what is on the glass and the
// front buffer (0x24) the frame before. It drives each pixel by back -> front, so
// callers must write every pixel that differs from the glass into the front buffer
// before the next refresh: writeAll() does, Watchface::draw() and the wake stub
// re-write their changed areas after a partial refresh. A full refresh doesn't swap
// (T80): the front buffer then already holds the frame, see lastRefreshFull().
void Display::refresh()
{
  if (!kState.firstRefreshDone) {
    // Draw the backbuffer as well on first refresh
    writeAll(true);
  }
  {
    auto powerLock = Power::Lock(Power::Flag::Display);
    _startTransfer();
    _transferCommand(0x20);
    _endTransfer();
    // Until the first refresh the controller keeps the FULL update from init()
    countRefresh(!kState.firstRefreshDone || kState.mode == FULL);
    if (!kState.firstRefreshDone)
      kState.backStale = false; // Both banks hold the frame (writeAll(true) above)

    waitWhileBusy();
  }
  if (!kState.firstRefreshDone) {
    _startTransfer();
    _setRefreshMode(kState.mode);
    _endTransfer();
    kState.firstRefreshDone = true;
  }

  // After a refresh, finalize the display inversion
  if (kState.postInvert) {
    kState.postInvert = false;
    _startTransfer();
    _transferCommand(0x21); // RAM for Display Update
    _transfer(kState.inverted ? 0b10001000 : 0b0); // Set both front/backbuffer
    _endTransfer();
  }
}

void Display::waitWhileBusy() {
  sSem = xSemaphoreCreateBinary();

  static constexpr gpio_config_t busy_conf = {
    .pin_bit_mask = 1ULL << HW::Display::Busy,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_LOW_LEVEL,
  };
  gpio_config(&busy_conf);
  // Setting the GPIO config reenables the interrupt

  // Set the wakeup on busy, in case tasks sleep the chip as well
  gpio_wakeup_enable((gpio_num_t)HW::Display::Busy, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  gpio_isr_handler_add((gpio_num_t)HW::Display::Busy, isr, (void*) 0);

  if (xSemaphoreTake(sSem, 10'000 / portTICK_PERIOD_MS) != pdTRUE) {
    ESP_LOGE("displ", "semaphore expired!");
  }

  gpio_isr_handler_remove((gpio_num_t)HW::Display::Busy);
  gpio_intr_disable((gpio_num_t)HW::Display::Busy);
  gpio_wakeup_disable((gpio_num_t)HW::Display::Busy);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

  vSemaphoreDelete(sSem);
}

void Display::setDarkBorder(bool dark) {
  if (kState.darkBorder == dark)
    return;
  _startTransfer();
  _transferCommand(0x3C); // BorderWavefrom
  _transfer(dark ? 0x02 : 0x05);
  _endTransfer();
  kState.darkBorder = dark;
}

void Display::setInverted(bool inverted) {
  if (kState.inverted == inverted)
    return;
  kState.inverted = inverted;
  _startTransfer();
  _transferCommand(0x21); // RAM for Display Update
  if (kState.firstRefreshDone) {
    _transfer(inverted ? 0b1000 : 0b10000000); // Only invert the frontbuffer
    kState.postInvert = true; // Queue the change for backbuffer
  } else {
    _transfer(inverted ? 0b10001000 : 0b0); // Set both front/backbuffer
  }
  _endTransfer();
}

void Display::alignRect(Rect& rect) const
{
  auto& [x, y, w, h] = rect;

  // Align x down, and w up
  w = (w + (x & 7) + 7) & ~7;
  x = x & ~7;

  // Align
  // x = x & ~7; // byte boundary
  // w = WIDTH - x < w ? WIDTH - x : w; // limit
  // h = HEIGHT - y < h ? HEIGHT - y : h; // limit

  // w = (w + 7) & ~7; // byte boundary, bitmaps are padded

  // w = x + w < WIDTH ? w : WIDTH - x; // limit
  // h = y + h < HEIGHT ? h : HEIGHT - y; // limit
}

// See getRamWrites(): 2 only in FULL mode after an odd number of partial refreshes
int Display::_ramWrites() const
{
  return getRamWrites();
}

void Display::writeAlignedRect(const Rect& rect)
{
  _startTransfer();
  for (int ram = 0; ram < _ramWrites(); ram++) {
    _setRamArea(rect);
    _transferCommand(ram ? 0x26 : 0x24);
    auto xst = rect.x >> 3;
    for (auto i = 0; i < rect.h; i++)
    {
      auto yoffset = (rect.y + i) * WB_BITMAP;
      _transfer(buffer + xst + yoffset, rect.w >> 3);
    }
  }
  _endTransfer();
}

void Display::writeAlignedRectPacked(const uint8_t* ptr, const Rect& rect)
{
  _startTransfer();
  for (int ram = 0; ram < _ramWrites(); ram++) {
    _setRamArea(rect);
    // ESP_LOGE("area","%p, %d %d %d %d, size %d", ptr, x, y, w, h, ((uint16_t)h) * w / 8);
    _transferCommand(ram ? 0x26 : 0x24);
    _transfer(ptr, ((uint16_t)rect.h * rect.w) >> 3);
  }
  _endTransfer();
}

void Display::writeRect(Rect rect)
{
  alignRect(rect);
  writeAlignedRect(rect);
}

void Display::writeAllAndRefresh()
{
  writeAll();
  refresh();
 }

void Display::writeAll(bool backbuffer)
{
  _startTransfer();
  for (int ram = 0; ram < (backbuffer ? 1 : _ramWrites()); ram++) {
    _setRamArea({0, 0, WIDTH, HEIGHT});
    _transferCommand(backbuffer || ram ? 0x26 : 0x24);
    _transfer(buffer, sizeof(buffer));
  }
  _endTransfer();
}

void Display::hibernate()
{
  _startTransfer();
  _transferCommand(0x10); // change deep sleep mode
  _transfer(0b01);  // mode 1: RAM and every register survive the HW reset on wake (T14-T26)
  // Mode 2 (0b11) loses the controller state within a minute (T50): it would need a
  // SWRESET-level re-init and both RAM banks rewritten on every wake
  _endTransfer();
}

Rect Display::getTextRect(const char * str, int16_t xc, int16_t yc) {
  int16_t x, y;
  uint16_t w, h;
  getTextBounds(str, xc < 0 ? cursor_x : xc, yc < 0 ? cursor_y : yc, &x, &y, &w, &h);
  return {static_cast<uint8_t>(x), static_cast<uint8_t>(y),
          static_cast<uint8_t>(w), static_cast<uint8_t>(h)};
}

void Display::drawPixel(int16_t x, int16_t y, uint16_t color)
{
  auto& ptr = buffer[(x >> 3) + y * WB_BITMAP];
  auto mask = 1 << (7 - (x & 7));
  if (color)
    ptr |= mask;
  else
    ptr &= ~mask;
  // ptr = (ptr & ~mask) | (-(color != 0) & mask); // Alternative
}

/**************************************************************************/
/*!
   @brief  Speed optimized vertical line drawing
   @param  x      Line horizontal start point
   @param  y      Line vertical start point
   @param  h      Length of vertical line to be drawn, including first point
   @param  color  Color to fill with
*/
/**************************************************************************/
void Display::drawFastVLine(int16_t x, int16_t y, int16_t h,
                               uint16_t color) {

  if (h < 0) { // Convert negative heights to positive equivalent
    h *= -1;
    y -= h - 1;
    if (y < 0) {
      h += y;
      y = 0;
    }
  }

  // Edge rejection (no-draw if totally off canvas)
  if ((x < 0) || (x >= width()) || (y >= height()) || ((y + h - 1) < 0)) {
    return;
  }

  if (y < 0) { // Clip top
    h += y;
    y = 0;
  }
  if (y + h > height()) { // Clip bottom
    h = height() - y;
  }

  uint8_t *ptr = &buffer[(x >> 3) + y * WB_BITMAP];
  if (color > 0) {
    uint8_t bit_mask = (0x80 >> (x & 7));
    for (int16_t i = 0; i < h; i++) {
      *ptr |= bit_mask;
      ptr += WB_BITMAP;
    }
  } else {
    uint8_t bit_mask = ~(0x80 >> (x & 7));
    for (int16_t i = 0; i < h; i++) {
      *ptr &= bit_mask;
      ptr += WB_BITMAP;
    }
  }
}

/**************************************************************************/
/*!
   @brief  Speed optimized horizontal line drawing
   @param  x      Line horizontal start point
   @param  y      Line vertical start point
   @param  w      Length of horizontal line to be drawn, including first point
   @param  color  Color to fill with
*/
/**************************************************************************/
void Display::drawFastHLine(int16_t x, int16_t y, int16_t w,
                               uint16_t color) {
  if (w < 0) { // Convert negative widths to positive equivalent
    w *= -1;
    x -= w - 1;
    if (x < 0) {
      w += x;
      x = 0;
    }
  }

  // Edge rejection (no-draw if totally off canvas)
  if ((y < 0) || (y >= height()) || (x >= width()) || ((x + w - 1) < 0)) {
    return;
  }

  if (x < 0) { // Clip left
    w += x;
    x = 0;
  }
  if (x + w >= width()) { // Clip right
    w = width() - x;
  }

  uint8_t *ptr = &buffer[(x >> 3) + y * WB_BITMAP];
  size_t remainingWidthBits = w;

  // check to see if first byte needs to be partially filled
  if ((x & 7) > 0) {
    // create bit mask for first byte
    uint8_t startByteBitMask = 0x00;
    for (int8_t i = (x & 7); ((i < 8) && (remainingWidthBits > 0)); i++) {
      startByteBitMask |= (0x80 >> i);
      remainingWidthBits--;
    }
    if (color > 0) {
      *ptr |= startByteBitMask;
    } else {
      *ptr &= ~startByteBitMask;
    }

    ptr++;
  }

  // do the next remainingWidthBits bits
  if (remainingWidthBits > 0) {
    size_t remainingWholeBytes = remainingWidthBits >> 3;
    size_t lastByteBits = remainingWidthBits & 7;
    uint8_t wholeByteColor = color > 0 ? 0xFF : 0x00;

    memset(ptr, wholeByteColor, remainingWholeBytes);

    if (lastByteBits > 0) {
      uint8_t lastByteBitMask = 0x00;
      for (size_t i = 0; i < lastByteBits; i++) {
        lastByteBitMask |= (0x80 >> i);
      }
      ptr += remainingWholeBytes;

      if (color > 0) {
        *ptr |= lastByteBitMask;
      } else {
        *ptr &= ~lastByteBitMask;
      }
    }
  }
}