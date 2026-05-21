/*
 * BLE Configuration Exchange for LightInk Watch
 * 
 * This module provides a simple BLE GATT server that allows a paired phone
 * to read and write configuration settings via JSON over a custom service.
 */

#include "ble.h"
#include "settings.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOSConfig.h"

/* NimBLE includes */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* JSON support */
#include <ArduinoJson.h>

#define TAG "BLE_CONFIG"

/* Custom Service UUIDs */
#define LIGHTINK_SERVICE_UUID           0x4c49, 0x4748, 0x5449, 0x4e4b, 0x0000, 0x0000, 0x0000, 0x0000
#define LIGHTINK_CONFIG_CHAR_UUID       0x4c49, 0x4748, 0x5449, 0x4e4b, 0x0000, 0x0000, 0x0000, 0x0001

/* Device name */
static const char *device_name = "LightInk";

/* BLE state */
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t config_char_handle = 0;
static bool config_exchanged = false;
static uint8_t ble_addr_type = 0;

/* Forward declarations */
static int ble_gap_event(struct ble_gap_event *event, void *arg);
static int config_char_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);

/* GATT characteristic definitions */
static const ble_uuid128_t lightink_service_uuid = BLE_UUID128_INIT(LIGHTINK_SERVICE_UUID);
static const ble_uuid128_t config_char_uuid = BLE_UUID128_INIT(LIGHTINK_CONFIG_CHAR_UUID);

static const ble_gatt_chr_def config_chars[] = {
    {
        .uuid = (ble_uuid_t *)&config_char_uuid,
        .access_cb = config_char_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
    },
    { 0 } /* No more characteristics */
};

static const ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (ble_uuid_t *)&lightink_service_uuid,
        .characteristics = config_chars,
    },
    { 0 } /* No more services */
};

/**
 * Serialize current settings to JSON
 */
static std::string serialize_config() {
    JsonDocument doc;
    
    doc["tz"] = kSettings.mTime.mMinutesWest;
    doc["beep"] = kSettings.mHourly.mBeep;
    doc["vib"] = kSettings.mHourly.mVib;
    doc["night"] = kSettings.mPower.mNight;
    doc["auto_pwr"] = kSettings.mPower.mAuto;
    doc["bat"] = kSettings.mBattery.mCurVoltage;
    
    std::string result;
    serializeJson(doc, result);
    return result;
}

/**
 * Deserialize JSON and apply to settings
 */
static bool deserialize_config(const uint8_t *data, size_t len) {
    JsonDocument doc;
    
    // Parse JSON from data
    DeserializationError error = deserializeJson(doc, (const char *)data, len);
    if (error) {
        ESP_LOGE(TAG, "JSON parse error: %s", error.c_str());
        return false;
    }
    
    // Apply settings
    if (doc.containsKey("tz"))
        kSettings.mTime.mMinutesWest = doc["tz"].as<int16_t>();
    if (doc.containsKey("beep"))
        kSettings.mHourly.mBeep = doc["beep"].as<bool>();
    if (doc.containsKey("vib"))
        kSettings.mHourly.mVib = doc["vib"].as<bool>();
    if (doc.containsKey("night"))
        kSettings.mPower.mNight = doc["night"].as<bool>();
    if (doc.containsKey("auto_pwr"))
        kSettings.mPower.mAuto = doc["auto_pwr"].as<bool>();
    
    ESP_LOGI(TAG, "Config updated from phone");
    return true;
}

/**
 * GATT characteristic access callback
 */
static int config_char_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    int rc = 0;
    
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        /* Phone is reading current config */
        std::string json = serialize_config();
        rc = os_mbuf_append(ctxt->om, (uint8_t *)json.c_str(), json.length());
        if (rc != 0) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        ESP_LOGI(TAG, "Config read by phone: %s", json.c_str());
    } 
    else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        /* Phone is writing new config */
        uint8_t *buf = os_mbuf_data(ctxt->om, NULL);
        uint16_t len = os_mbuf_len(ctxt->om);
        
        if (deserialize_config(buf, len)) {
            config_exchanged = true;
            ESP_LOGI(TAG, "Config written by phone");
        } else {
            rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
    }
    
    return rc;
}

/**
 * GAP event handler
 */
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "Connection failed; status=%d", event->connect.status);
        } else {
            conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connection established; handle=%d", conn_handle);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected; reason=%d", event->disconnect.reason);
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising complete");
        break;

    default:
        break;
    }

    return 0;
}

/**
 * BLE host sync callback
 */
static void ble_on_sync(void) {
    int rc;

    rc = ble_hs_id_infer_auto(0, &ble_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer address type; rc=%d", rc);
        return;
    }

    uint8_t addr[6] = {0};
    rc = ble_hs_id_copy_addr(ble_addr_type, addr, NULL);
    ESP_LOGI(TAG, "Device Address: %02x:%02x:%02x:%02x:%02x:%02x", 
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

/**
 * BLE host reset callback
 */
static void ble_on_reset(int reason) {
    ESP_LOGE(TAG, "BLE reset; reason=%d", reason);
}

/**
 * Start BLE advertising
 */
static void ble_advertise(void) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    fields.name = (uint8_t *)device_name;
    fields.name_len = strlen(device_name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set advertisement data; rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    
    rc = ble_gap_adv_start(ble_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start advertising; rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "Advertising started");
}

/**
 * BLE host task (runs on core 1)
 */
static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/**
 * Initialize BLE stack
 */
void ble_init() {
    int rc;

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize NimBLE */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init nimble; rc=%d", ret);
        return;
    }

    /* Configure BLE host */
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    /* Register GATT services */
    rc = ble_gatts_count_cfg(gatt_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to count GATT services; rc=%d", rc);
        return;
    }

    rc = ble_gatts_add_svcs(gatt_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to add GATT services; rc=%d", rc);
        return;
    }

    /* Set device name */
    rc = ble_svc_gap_device_name_set(device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set device name; rc=%d", rc);
        return;
    }

    /* Start BLE host task */
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE initialized");
}

/**
 * Synchronize configuration with phone
 * Advertises and waits for connection + config exchange
 */
bool ble_sync(uint32_t timeout_ms) {
    if (ble_hs_is_enabled() == 0) {
        ESP_LOGE(TAG, "BLE not initialized");
        return false;
    }

    config_exchanged = false;
    conn_handle = BLE_HS_CONN_HANDLE_NONE;

    /* Start advertising */
    ble_advertise();

    /* Wait for config exchange or timeout */
    uint32_t start = millis();
    while (!config_exchanged && (timeout_ms == 0 || millis() - start < timeout_ms)) {
        delay(100);
    }

    /* Stop advertising */
    ble_gap_adv_stop();

    /* Disconnect if connected */
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        delay(500); /* Give time for disconnect to complete */
    }

    bool result = config_exchanged;
    ESP_LOGI(TAG, "BLE sync %s", result ? "successful" : "timeout");
    return result;
}

/**
 * Shutdown BLE stack
 */
void ble_deinit() {
    /* Stop advertising */
    ble_gap_adv_stop();

    /* Disconnect if connected */
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        delay(500);
    }

    /* Stop NimBLE */
    nimble_port_stop();

    ESP_LOGI(TAG, "BLE deinitialized");
}
