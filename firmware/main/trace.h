#pragma once

#include "esp_timer.h"
#include "esp_log.h"
#include <inttypes.h>

// Enable/disable speed tracing globally
constexpr bool TRACE_SPEED = true;

/**
 * @brief RAII-based trace scope for measuring execution time
 * 
 * Usage:
 *   {
 *     TRACE("my_operation");
 *     // ... code to measure ...
 *   } // Automatically logs elapsed time on scope exit
 */
struct TraceScope {
    const char* tag;
    int64_t start;
    
    explicit TraceScope(const char* t) 
        : tag(t), start(TRACE_SPEED ? esp_timer_get_time() : 0) {}
    
    ~TraceScope() {
        if (TRACE_SPEED) {
            int32_t elapsed = static_cast<int32_t>(esp_timer_get_time() - start);
            ESP_LOGE("TRACE", "%-30s %" PRId32 " us", tag, elapsed);
        }
    }
};

/**
 * @brief Macro to create a trace scope with automatic line-based naming
 * 
 * Usage:
 *   TRACE("operation_name");
 *   // ... code to measure ...
 */
#define TRACE(name) TraceScope _trace_scope_##__LINE__(name)
