cmake_minimum_required(VERSION 3.22)

project(genie_native LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "genie_native must be built with the Android NDK toolchain")
endif()

if(NOT "${ANDROID_ABI}" STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Only arm64-v8a is supported; got '${ANDROID_ABI}'")
endif()

set(GENIE_SOURCES genie_native.cpp genie_bridge.cpp genie_model.cpp genie_content.cpp)
set(GENIE_TARGET genie_native)

add_library(${GENIE_TARGET} SHARED ${GENIE_SOURCES})

target_compile_features(${GENIE_TARGET} PRIVATE cxx_std_17)
target_compile_options(${GENIE_TARGET} PRIVATE
    -fno-exceptions
    -fno-rtti
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
    -Werror
)

target_link_options(${GENIE_TARGET} PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map"
    -Wl,--gc-sections
    -Wl,--no-undefined
    -Wl,-z,relro
    -Wl,-z,now
)

target_link_libraries(${GENIE_TARGET} PRIVATE android log dl binder_ndk)

set_target_properties(${GENIE_TARGET} PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "${GENIE_TARGET}"
)

# Loaded only by the central system_server entry, never by target applications.
add_library(genie_content SHARED genie_content.cpp genie_model.cpp)
target_compile_features(genie_content PRIVATE cxx_std_17)
target_compile_options(genie_content PRIVATE
    -fno-exceptions
    -fno-rtti
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -Wall
    -Wextra
    -Werror
)
target_link_options(genie_content PRIVATE
    -Wl,--no-undefined
    -Wl,-z,relro
    -Wl,-z,now
)
target_link_libraries(genie_content PRIVATE android log dl)
set_target_properties(genie_content PROPERTIES CXX_EXTENSIONS OFF)
