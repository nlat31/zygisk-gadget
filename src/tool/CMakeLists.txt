cmake_minimum_required(VERSION 3.18.1)

if (NOT DEFINED TOOL_NAME)
    message(FATAL_ERROR "TOOL_NAME is not set")
endif ()

if (NOT DEFINED MODULE_DIR)
    message(FATAL_ERROR "MODULE_DIR is not set")
endif ()

# Generate logcat.h with correct config path (portable; no sed in-place edits).
set(CONFIG_FILE_PATH "/data/adb/modules/${MODULE_DIR}/config")
set(GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${GENERATED_INCLUDE_DIR}")
configure_file(
        "${CMAKE_SOURCE_DIR}/src/include/logcat.h.in"
        "${GENERATED_INCLUDE_DIR}/logcat.h"
        @ONLY
)

message("Build type: ${CMAKE_BUILD_TYPE}")

set(CMAKE_CXX_STANDARD 20)

set(LINKER_FLAGS "-ffixed-x18 -Wl,--hash-style=both")
set(C_FLAGS "-fdata-sections -ffunction-sections")
set(CXX_FLAGS "${CXX_FLAGS} -fno-exceptions -fno-rtti")

if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(C_FLAGS "${C_FLAGS} -O2 -fvisibility=hidden -fvisibility-inlines-hidden")
    set(LINKER_FLAGS "${LINKER_FLAGS} -Wl,-exclude-libs,ALL -Wl,--gc-sections -Wl,--strip-all")
else ()
    set(C_FLAGS "${C_FLAGS} -O0")
endif ()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${C_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${C_FLAGS} ${CXX_FLAGS}")

set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${LINKER_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${LINKER_FLAGS}")

add_executable(${TOOL_NAME} main.cpp logcat.cpp)
target_include_directories(${TOOL_NAME} BEFORE PRIVATE "${GENERATED_INCLUDE_DIR}")
target_link_libraries(${TOOL_NAME} log)

if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_custom_command(TARGET ${TOOL_NAME} POST_BUILD
            COMMAND ${CMAKE_STRIP} --strip-all --remove-section=.comment "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${TOOL_NAME}")
endif ()