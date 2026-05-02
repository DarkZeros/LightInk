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
    time.cpp
    menus.cpp
    ui.cpp

    watchface.cpp
    watchface_default.cpp

    deep_sleep.cpp
    deep_sleep_utils.cpp

    uspi.cpp

    secrets.cpp

    # WIP stuff
    # ble.cpp

    gps.cpp
    radio.cpp
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${includedirs} REQUIRES ${requires} PRIV_REQUIRES ${privrequires})

#set_property(TARGET ${COMPONENT_TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
#set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
target_compile_options(${COMPONENT_TARGET} PUBLIC
    #-flto
    -Wno-volatile
    -fpermissive
)