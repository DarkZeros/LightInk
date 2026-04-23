#pragma once

#include <vector>
#include <cstdint>
#include <array>

#include "hardware.h"
#include "input.h"

// Forward declaration to avoid circular includes
struct TouchSettings;

/**
 * @brief Button-based input implementation
 * 
 * Handles standard GPIO interrupt-based button input as an alternative to touch sensors.
 * Implements the InputInterface to be compatible with the rest of the system.
 * Uses the same TouchSettings configuration as Touch input.
 */
class ButtonInput : public InputInterface {
private:
    TouchSettings& mSettings;

public:
    explicit ButtonInput(TouchSettings& settings);

    void initialize() override;
    void setUp(bool onlyMenuLight) override;

    std::vector<uint16_t> readAll() const override;
    Btn read() const override;
    void clear() const override;
    Btn readAndClear() const override;

    void enable() const override;
    void disable() const override;
};
