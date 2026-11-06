# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Always use ccache. idf.py passes CCACHE_ENABLE from the environment, and a
# build without it would change every compile command and rebuild everything.
set(CCACHE_ENABLE 1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Share the cache between checkouts (git worktrees, project copies). With -g the
# build directory gets into the ccache hashes twice: as the working directory,
# so it is mapped to a fixed name in the debug info (which also keeps a reused
# object from pointing at the tree that compiled it first), and as a line marker
# in the preprocessor output, which -fno-working-directory leaves out.
idf_build_set_property(COMPILE_OPTIONS "-fdebug-prefix-map=${CMAKE_BINARY_DIR}=./build" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-fno-working-directory" APPEND)

# The only HTTP request is plain http to ip-api.com. Without this, Arduino's
# HTTPClient links NetworkClientSecure and with it the whole TLS stack.
add_compile_definitions(HTTPCLIENT_NOSECURE)

project(LightInk)

if(CCACHE_FOUND)
    # The compile commands have absolute paths. CCACHE_BASEDIR makes ccache see
    # the ones of this checkout relative to the build directory. The IDF is left
    # out, it stays absolute so that worktrees building with the IDF of the main
    # checkout share its objects with it.
    set(ccache_basedir "${CMAKE_SOURCE_DIR}/main:${CMAKE_SOURCE_DIR}/components")
    string(APPEND ccache_basedir ":${CMAKE_SOURCE_DIR}/managed_components:${CMAKE_BINARY_DIR}")
    set(ccache_env "env CCACHE_BASEDIR=${ccache_basedir} CCACHE_ABSSTDERR=1")
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${ccache_env} ${CCACHE_FOUND}")

    # RadioLib's BuildOpt.h mentions __DATE__ and __TIME__ in a macro, and ccache
    # then goes without its direct mode for every file that includes it, which
    # is all of RadioLib and most of main. Only RadioLib's Module.cpp expands
    # the macro, into a debug string, so the time can be ignored there. The other
    # components keep the default, esp_app_desc.c has the real compile time.
    foreach(component main radioLib)
        idf_component_get_property(component_lib ${component} COMPONENT_LIB)
        set_property(TARGET ${component_lib} PROPERTY RULE_LAUNCH_COMPILE
            "${ccache_env} CCACHE_SLOPPINESS=time_macros ${CCACHE_FOUND}")
    endforeach()
endif()
