#pragma once

#include <cstdint>

/**
 * Initialize the BLE stack.
 * Must be called before ble_sync().
 */
void ble_init();

/**
 * Start BLE advertising and wait for config exchange with a paired phone.
 * 
 * @param timeout_ms Maximum time to advertise and wait for connection/exchange.
 *                   If 0, waits indefinitely.
 * @return true if config exchange completed successfully, false on timeout or error.
 */
bool ble_sync(uint32_t timeout_ms = 10000);

/**
 * Shutdown the BLE stack.
 * Should be called after ble_sync() completes.
 */
void ble_deinit();
