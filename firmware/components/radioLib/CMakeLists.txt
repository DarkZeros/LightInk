set(includedirs
    RadioLib/src
)

set(requires
)

set(privrequires
    arduino-esp32
)

# Everything but the HALs of other platforms
file(GLOB_RECURSE srcs RELATIVE ${CMAKE_CURRENT_LIST_DIR} RadioLib/src/*.cpp)
list(FILTER srcs EXCLUDE REGEX "/hal/(ESP-IDF|RPiPico|Stm32duino)/")

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${includedirs} REQUIRES ${requires} PRIV_REQUIRES ${privrequires})
