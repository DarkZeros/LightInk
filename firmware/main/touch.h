#pragma once

#include <array>
#include <cstdint> 

#include <magic_enum.hpp>

#include "hardware.h"
#include "input.h"

enum MeasureRate {
    // _125ms = 1, // Extremely fast
    _250ms = 2,
    _500ms = 4,
    _1s = 8,
    _2s = 15, // 16 would overflow
    // _4s = 32, // Not possible, limit is 2s
};

enum MeasureCycles {
    _31ms = 1,  // 50-60 -> 30-35 // 33-40 -> 20-25
    _62ms = 2,  // 110 -> 60
};

struct TouchSettings {
  // More cycles more accurate, and more power
  // More often checks, also more power
  MeasureCycles mCycles[2] = {_31ms, _31ms};
  MeasureRate mRate[2] = {_250ms, _1s};
  bool mHaptic {false};
  int8_t mSensitivity {60};
  std::array<uint8_t, 4> mMap{{0,1,2,3}};

  struct Setup {
    bool mMode : 1 {false};
    bool mPower : 1 {false};
    auto operator<=>(const Setup&) const = default;
  };
  std::optional<Setup> mSetup{};
};

class Touch : public InputInterface {
private:
    TouchSettings& mSettings;
public:
    explicit Touch(TouchSettings& settings);

    void initialize() override;
    void setUp(bool onlyMenuLight) override;

    std::vector<uint16_t> readAll() const override;
    Btn read() const override;
    void clear() const override;
    Btn readAndClear() const override;

    void enable() const override;
    void disable() const override;
};
