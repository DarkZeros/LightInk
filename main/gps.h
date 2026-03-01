#include <chrono>
#include <optional>
#include <TimeLib.h>

#include "hardware.h"

/* Helper class to read/configure GPS as well as
 * turning it on/off and keeping it in deepsleep
 */
class Gps {
public:
  static struct Data {
    struct Location {
      double mLat;
      double mLon;
    };
    struct DateTime {
      tmElements_t mElements;
      uint8_t mCentiSeconds;
    };
    std::optional<Location> mLocation;
    std::optional<DateTime> mDateTime;
    std::optional<float> mSpeed;
    std::optional<float> mDirection;
    std::optional<std::chrono::seconds> mTimeOn;
  } mData;

  static constexpr std::chrono::seconds kMaxTimeGpsOn {60};
  static constexpr std::chrono::seconds kGpsUpdateRate {10};

  void on() const { set(true); }
  void off() const { set(false); }
  void set(bool high) const;
  bool isOn() const;

  bool read(uint32_t timeout = 200) const;
};
