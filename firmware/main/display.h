// based on Demo Example from Good Display, available here: http://www.e-paper-display.com/download_detail/downloadsId=806.html
// Panel: GDEH0154D67 : http://www.e-paper-display.com/products_detail/productId=455.html
// Controller : SSD1681 : http://www.e-paper-display.com/download_detail/downloadsId=825.html
//
// Inspired on GxEPD2 by Author: Jean-Marc Zingg
// Library: https://github.com/ZinggJM/GxEPD2
//
// Fully rewriten for this project

#pragma once

#include <Adafruit_GFX.h>

#include "esp_attr.h"

#include "hardware.h"

#include "lut.h"

enum DisplayMode {
  FULL,
  FAST,
  GOOD,  // 6_1 // Might burn the display after days/weeks...
  QUICK, // 2_1
  // REPAIR, // Use overvoltage, and keep it long time one direction then the other
};

struct DisplaySettings {
  // Settings that can be changed
  bool mInvert {false};
  bool mDarkBorder {false};
  uint8_t mRotation {2};
  DisplayMode mMenuLut {FAST};
  DisplayMode mWatchLut {FAST};
};

extern int getSetDisplayMode();
extern uint8_t getHwRotation();
extern int getRamWrites(); // RAM banks each write must go to (1 or 2), also in the wake stub
extern void countRefresh(bool full); // After every 0x20 that refreshes, also in the wake stub

struct Rect {
  uint8_t x, y, w, h;
  uint16_t size() const {return static_cast<uint16_t>(w)/8 * h; };
};

class Display : public Adafruit_GFX {
public:
  static constexpr bool kReduceBoosterTime = false; // Saves ~200ms + Reduce power usage, degrades quality!
  static constexpr bool kFastUpdateTemp = true; // Saves 5ms + FixedSpeedier LUT (300ms update)
  static constexpr bool kOverdriveSPI = false; // Uses a 25% faster SPI out of spec
  static constexpr bool kArduinoSPI = false; // Use the arduino SPI or the uSPI

  static constexpr uint8_t WIDTH = 200;
  static constexpr uint8_t HEIGHT = WIDTH;
  static constexpr uint8_t WB_BITMAP = (WIDTH + 7) / 8;

  uint8_t buffer[WB_BITMAP * HEIGHT];

  Display();

  void init();

  void setRotation(uint8_t r) override;
  void toBuffer(Rect& rect) const; // View rect -> buffer rect (transposed for 90/270)
  void alignRect(Rect& rect) const;
  Rect getTextRect(const char * str, int16_t xc = -1, int16_t yc = -1);

  // Heavily optimize this please
  void drawPixel(int16_t x, int16_t y, uint16_t color) override;
  void fillScreen(uint16_t color) override
  {
    memset(buffer, (color) ? 0xFF : 0x00, sizeof(buffer));
  }
  // Optimized for our case from Canvas1
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override;
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override;
  void drawFastRawVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
  void drawFastRawHLine(int16_t x, int16_t y, int16_t w, uint16_t color);

  void setDarkBorder(bool darkBorder);
  void setInverted(bool inverted);

  void setRefreshMode(DisplayMode mode);
  DisplayMode getRefreshMode() const;
  void refresh();
  // The last refresh was full: the front buffer holds the glass, no re-write needed
  bool lastRefreshFull() const;
  void hibernate();
  void waitWhileBusy();
  void writeRect(Rect rect);
  void writeAlignedRect(const Rect& rect);
  void writeAlignedRectPacked(const uint8_t* ptr, const Rect& rect);
  void writeAll(bool backBuffer = false);
  void writeAllAndRefresh();

  // Aditional helper draw
  void drawMoon(float p, uint16_t x, uint16_t y, uint16_t radius, uint16_t on, uint16_t off);
  void fillCalcEllipse(int x, int y, uint8_t width, uint8_t height, uint16_t on);
  void fillEllipseDifference(int x, int y, uint8_t width1, uint8_t width2, uint8_t height, bool big, uint16_t color);
  void drawMoonFast(float p, int x, int y, uint8_t r, uint16_t on, uint16_t off);

private:
  int _ramWrites() const; // RAM banks each write goes to, see getRamWrites()

  // Called internally in middle of transfer
  void _setRamArea(const Rect& rect);
  void _setRefreshMode(const DisplayMode& mode);
  void _setCustomLut(const DisplayMode& mode);
  void _loadRomLut(const DisplayMode& mode);

  // SPI transfer methods
  void _startTransfer();
  void _transfer(uint8_t value);
  void _transfer(const uint8_t* value, size_t size);
  void _transferCommand(uint8_t c);
  void _endTransfer();
};

// Rotation in hardware (docs/display_findings.md T27, T52): the software always draws
// at 0 degrees, into a buffer that is transposed for 90/270 (a byte then holds 8
// vertical pixels), and the entry mode mirrors X for 90/180 and Y for 180/270 while
// the controller writes. Buffer rect (x along the bytes) -> RAM window, from its start
// corner. Shared by the driver and the wake stub.
struct RamWindow { uint8_t xs, xe, ys, ye; };
FORCE_INLINE_ATTR RamWindow ramWindow(uint8_t rotation, uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
  const bool mirrorX = rotation == 1 || rotation == 2, mirrorY = rotation >= 2;
  constexpr int W = Display::WIDTH, H = Display::HEIGHT;
  return {
    uint8_t(mirrorX ? (W - 1 - x) >> 3 : x >> 3),
    uint8_t(mirrorX ? (W - x - w) >> 3 : (x + w - 1) >> 3),
    uint8_t(mirrorY ? H - 1 - y : y),
    uint8_t(mirrorY ? H - y - h : y + h - 1),
  };
}