set(includedirs
)

set(requires
    WiFiManager
    NTPClient
    ArduinoJson
    ArduinoNvs
    GfxLibrary
    Time
    magic_enum
    sunset
    radioLib
    esp_hal_wdt
    esp_hal_touch_sens
    bt
    nvs_flash
)

set(privrequires
)

set(srcs 
    main.cpp
    display.cpp
    display_draw.cpp
    battery.cpp
    core.cpp
    power.cpp
    light.cpp
    touch.cpp
    peripherals.cpp
    beep.cpp
    time.cpp
    menus.cpp
    ui.cpp

    watchface.cpp
    watchface_default.cpp
    watchface_eclipse.cpp

    deep_sleep.cpp
    deep_sleep_utils.cpp

    uspi.cpp
    trace.cpp
    display_test.cpp

    secrets.cpp
    test_signals.cpp

    ble.cpp
    settings_table.cpp
    storage.cpp

    gps.cpp
    radio.cpp
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# secrets.cpp (WiFi networks, radio signals) is local and gitignored: a fresh clone
# starts from a copy of the defaults. An existing one is never touched
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.cpp")
    file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/secrets.default.cpp"
                   "${CMAKE_CURRENT_LIST_DIR}/secrets.cpp")
    message(NOTICE "main/secrets.cpp created from secrets.default.cpp: "
                   "put your WiFi networks and radio signals there")
endif()

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${includedirs} REQUIRES ${requires} PRIV_REQUIRES ${privrequires})

#set_property(TARGET ${COMPONENT_TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
#set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
target_compile_options(${COMPONENT_TARGET} PRIVATE
    #-flto
    -Wno-volatile
    -fpermissive
)

# NimBLE (as central) starts a pairing by itself when the phone answers "key missing".
# Only the Pair menu may pair: a sync then ends with NeedsPair, and Android isn't asked
# for a code by every sync. The option has no Kconfig entry
idf_component_get_property(bt_lib bt COMPONENT_LIB)
target_compile_definitions(${bt_lib} PRIVATE MYNEWT_VAL_BLE_RESTART_PAIR=0)
