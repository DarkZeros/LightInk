set(includedirs
    Time
)

set(requires
)

set(privrequires 
    arduino-esp32
)

set(srcs 
    Time/DateStrings.cpp
    Time/Time.cpp
)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${includedirs} REQUIRES ${requires} PRIV_REQUIRES ${privrequires})


# TimeLib.h only recognises the newlib guard and would redefine time_t otherwise
if(CONFIG_LIBC_PICOLIBC)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC __time_t_defined)
endif()
