#pragma once

#include <vector>
#include <cstdint>

/**
 * @brief Pure virtual interface for input handling (touch or buttons)
 * 
 * Both Touch and ButtonInput implementations must conform to this interface.
 * This allows hardware-specific input selection at compile-time while keeping
 * the rest of the codebase unchanged.
 */
class InputInterface {
public:
    virtual ~InputInterface() = default;

    enum Btn {
        NONE = 0,
        DOWN = 0b0001,
        UP   = 0b0010,
        MENU = 0b0100,
        BACK = 0b1000,
        // Alias
        LIGHT = BACK,
    };

    /**
     * @brief Initialize the input hardware
     */
    virtual void initialize() = 0;

    /**
     * @brief Set up the input for operation
     * @param onlyMenuLight If true, only enable menu and light buttons
     */
    virtual void setUp(bool onlyMenuLight) = 0;

    /**
     * @brief Read current button state without clearing
     * @return Button state bitmask
     */
    virtual Btn read() const = 0;

    /**
     * @brief Read all raw input values (for calibration/debugging)
     * @return Vector of raw input values
     */
    virtual std::vector<uint16_t> readAll() const = 0;

    /**
     * @brief Clear the input state/interrupt flags
     */
    virtual void clear() const = 0;

    /**
     * @brief Read button state and clear it atomically
     * @return Button state bitmask
     */
    virtual Btn readAndClear() const = 0;

    /**
     * @brief Enable input wakeup from deep sleep
     */
    virtual void enable() const = 0;

    /**
     * @brief Disable input wakeup from deep sleep
     */
    virtual void disable() const = 0;
};
