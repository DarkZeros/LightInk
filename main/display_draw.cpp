#include "display.h"

// My version of calculate ellipse endpoints faster, but unstable!
template<typename T = uint16_t>
std::vector<std::pair<T, T>> calcEllipse_(int x, int y, uint8_t width, uint8_t height)
{
    int x1 = -width, y1 = 0; // II quadrant from bottom left to top right
    int e2 = height, dx = (1 + 2 * x1) * e2 * e2; // error increment
    int dy = x1 * x1, err = dx + dy; // error of 1 step

    std::vector<std::pair<T, T>> lines;
    lines.reserve(height);

    do {
        e2 = 2 * err;

        if (e2 >= dx) {
            x1++;
            err += dx += 2 * height * height;
        } // x1 step

        if (e2 <= dy) {
            // Whenever we advance to next line we print the previous line
            lines.emplace_back(y1, -x1);
            y1++;
            err += dy += 2 * width * width;
        } // y1 step
    } while (x1 <= 0);

    // -> finish tip of ellipse ?
    // lines.emplace_back(y1, 0);
    return lines;
}
template<typename T = uint16_t>
std::vector<std::pair<T, T>> calcEllipse(int16_t x0, int16_t y0, int16_t rw, int16_t rh)
{
  // Bresenham's ellipse algorithm
  int16_t x = 0, y = rh;
  int32_t rw2 = rw * rw, rh2 = rh * rh;
  int32_t twoRw2 = 2 * rw2, twoRh2 = 2 * rh2;

  int32_t decision = rh2 - (rw2 * rh) + (rw2 / 4);

  std::vector<std::pair<T, T>> lines;
  lines.reserve(rh);

  // region 1
  while ((twoRh2 * x) < (twoRw2 * y)) {
    x++;
    if (decision < 0) {
      decision += rh2 + (twoRh2 * x);
    } else {
      decision += rh2 + (twoRh2 * x) - (twoRw2 * y);
      lines.emplace_back(y, x - 1);
      y--;
    }
  }

  // region 2
  decision = ((rh2 * (2 * x + 1) * (2 * x + 1)) >> 2) +
             (rw2 * (y - 1) * (y - 1)) - (rw2 * rh2);
  while (y >= 0) {
    lines.emplace_back(y, x);

    y--;
    if (decision > 0) {
      decision += rw2 - (twoRw2 * y);
    } else {
      decision += rw2 + (twoRh2 * x) - (twoRw2 * y);
      x++;
    }
  }
  return lines;
}

// Unused
void Display::fillCalcEllipse(int x, int y, uint8_t width, uint8_t height, uint16_t on)
{
  for (auto& [y1, x1] : calcEllipse(x, y, width, height)) {
      drawLine(x + x1, y - y1, x - x1, y - y1, on);
      drawLine(x + x1, y + y1, x - x1, y + y1, on);
  }
}

// Unused
void Display::fillEllipseDifference(int x, int y, uint8_t width1, uint8_t width2, uint8_t height, bool big, uint16_t color)
{
  auto elipse1 = calcEllipse(x, y, width1, height);
  auto elipse2 = calcEllipse(x, y, width2, height);

  for (auto i = 0; i < elipse1.size(); i++) {
    auto& [y11, x11] = elipse1[i];
    auto& [y12, x12] = elipse2[i];
    // ESP_LOGE("", "%d %d %d %d", y11, y12, x11, x12);
    if (big) {
      drawLine(x - x11, y - y11, x + x12, y - y11, color);
      drawLine(x - x11, y + y11, x + x12, y + y11, color);
    } else {
      drawLine(x + x11, y - y11, x + x12, y - y11, color);
      drawLine(x + x11, y + y11, x + x12, y + y11, color);
    }
  }
}

// In theory a faster way to draw moon is to calculate a elipse coords
// And draw a circle half B/W or W/B using the elipse coordinate as mid point
// Takes around 220-280us vs 430us the original one!
void Display::drawMoonFast(float p, int x, int y, uint8_t r, uint16_t on, uint16_t off)
{
  bool color = p <= 50 ? on : off;
  auto w = p < 50 ? p : p - 50;
  bool left = w >= 25;
  w = w < 25 ? 25 - w : w - 25;
  int er = r * w / 25;
 
  if (er == 0) {
    // This happens whe the elipse is 0, just half circle!
    fillCircleHelper(x, y, r, p < 50 ? 0b10 : 0b01, 0, on);
    fillCircleHelper(x, y, r, p < 50 ? 0b01 : 0b10, 0, off);
    return;
  }

  auto inner = calcEllipse<uint8_t>(x, y, er, r);
  auto outter = calcEllipse<uint8_t>(x, y, r, r);
  
  if (outter.size() != inner.size()) {
    ESP_LOGE("drawMoonFast", "outter %d != inner %d", outter.size(), inner.size());
    // drawMoon(p, x, y, r, on, off);
    return;
  }

  for (auto i = 0; i < outter.size(); i++) {
    auto& [y11, x11] = outter[i];
    auto& [y12, x12] = inner[i];
    auto mid = left ? -x12 : x12;
    auto w1 = mid - 1 + x11;
    auto w2 = x11 - mid;
    if (w1 > 0) {
      drawFastHLine(x - x11, y - y11, w1, color);
      drawFastHLine(x - x11, y + y11, w1, color);
    }
    if (w2 > 0) {
      drawFastHLine(x + mid, y - y11, w2, !color);
      drawFastHLine(x + mid, y + y11, w2, !color);
    }
  }
}

void Display::drawMoon(float p, uint16_t x, uint16_t y, uint16_t radius, uint16_t on, uint16_t off) 
{
  // Write half circle on/off based on the phase
  startWrite();
  fillCircleHelper(x, y, radius, p < 50 ? 0b10 : 0b01, 0, on);
  fillCircleHelper(x, y, radius, p < 50 ? 0b01 : 0b10, 0, off);
  endWrite();
  
  // Then depending on the phase, fill an elipse for the rest
  auto w = p < 50 ? p : p - 50;
  w = w < 25 ? 25 - w : w - 25;
  bool color = (p < 25 || p >= 75) ? on : off;
  fillEllipse(x, y, radius * w / 25, radius, color);
}