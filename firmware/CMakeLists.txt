# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The only HTTP request is plain http to ip-api.com. Without this, Arduino's
# HTTPClient links NetworkClientSecure and with it the whole TLS stack.
add_compile_definitions(HTTPCLIENT_NOSECURE)

project(LightInk)
