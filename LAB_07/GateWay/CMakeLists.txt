# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Keep the OpenThread CLI CoAP client and its Observe sub-command enabled in
# this project build. Observe is disabled by upstream OpenThread by default.
idf_build_set_property(COMPILE_OPTIONS "-DOPENTHREAD_CONFIG_COAP_API_ENABLE=1" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DOPENTHREAD_CONFIG_COAP_OBSERVE_API_ENABLE=1" APPEND)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(Lab2)
