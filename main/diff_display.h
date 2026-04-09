#pragma once

#include "display.h"
#include <span>

// Class to capture any drawPixel call, 
// then serialize the diff bytes to a buffer
class DiffDisplay : public Display {
public:
  void snapshot();
  std::optional<int> getDelta(
    std::span<const uint8_t> current,
    std::span<uint8_t> outBuffer
  );

private:
  uint8_t mPrev[WB_BITMAP * HEIGHT]{};
};
