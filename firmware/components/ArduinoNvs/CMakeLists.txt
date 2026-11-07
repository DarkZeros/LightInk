set(includedirs
    ArduinoNvs/src
)

set(requires
    nvs_flash
)

set(privrequires
    arduino-esp32
)

set(srcs 
    ArduinoNvs/src/ArduinoNvs.cpp 
)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${includedirs} REQUIRES ${requires} PRIV_REQUIRES ${privrequires})

# Upstream prints uint32_t with %u/%X, which is long on Xtensa
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format)
