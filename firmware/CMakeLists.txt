# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Always use ccache. idf.py passes CCACHE_ENABLE from the environment, and a
# build without it would change every compile command and rebuild everything.
set(CCACHE_ENABLE 1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(LightInk)
