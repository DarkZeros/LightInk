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

    ble.cpp
    settings_table.cpp
    storage.cpp

    gps.cpp
    radio.cpp
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${includedirs} REQUIRES ${requires} PRIV_REQUIRES ${privrequires})

#set_property(TARGET ${COMPONENT_TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
#set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
target_compile_options(${COMPONENT_TARGET} PRIVATE
    #-flto
    -Wno-volatile
    -fpermissive
)