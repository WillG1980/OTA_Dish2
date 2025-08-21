# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)
add_compile_options(-Wno-missing-field-initializers)
add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wno-unused-variable>)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-variable>)

add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wno-deprecated-declarations>)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-declarations>)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Get git tag
execute_process(
  COMMAND git rev-list --count HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_TAG
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

math(EXPR APP_VERSION "${GIT_TAG} + 1")

# Set project name
set(PROJECT_NAME_STR "ota-dishwasher")

# Pass to compiler
add_compile_definitions(
  OTA_VERSION="${APP_VERSION}"
  VERSION="${APP_VERSION}"
  APP_VERSION="${APP_VERSION}"
  PROJECT_NAME="${PROJECT_NAME_STR}"
 # TAG="Dishwasher"
  WIFI_SSID_REAL="House619"
  WIFI_PASS_REAL="Wifi6860"

)

project(${PROJECT_NAME_STR})

