# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)
add_compile_options(-Wno-missing-field-initializers)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Get git tag
execute_process(
  COMMAND git rev-list --count HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_TAG
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

math(EXPR APP_VERSION "${GIT_TAG} + 1")

#set(APP_VERSION "${PENDING_VERSION}")
#if(APP_VERSION LESS 10)
    #set(APP_VERSION "000${PENDING_VERSION}")
#elseif(APP_VERSION LESS 100)
    #set(APP_VERSION "00${PENDING_VERSION}")
#elseif(APP_VERSION LESS 1000)
    #set(APP_VERSION "0${PENDING_VERSION}")
#endif()

# Set project name
set(PROJECT_NAME_STR "ota-dishwasher")

# Pass to compiler
add_compile_definitions(
  OTA_VERSION="${APP_VERSION}"
  VERSION="${GIT_TAG}"
  PROJECT_NAME="${PROJECT_NAME_STR}"
 # TAG="Dishwasher"
<<<<<<< HEAD
  WIFI_SSID_REAL="House619"
  WIFI_PASS_REAL="Wifi6860"

=======
>>>>>>> ecef6b0d4d9f7b4ad0e2dea07c6d2b948dae4cbd
)

project(${PROJECT_NAME_STR})

