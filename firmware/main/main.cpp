
#include "core.h"
#include "display_test.h"
#include "esp_pm.h"

Core& getInstance() {
    static Core instance; // Lazily initialized on first call
    return instance;
}

extern "C" {
void app_main(void) {
    // Enable PM for auto sleep while executing
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = true
    };
    esp_pm_configure(&pm_config);

    DisplayTest::run(); // Debug only, never returns when enabled
    getInstance();
}
}