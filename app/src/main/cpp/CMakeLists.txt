cmake_minimum_required(VERSION 3.22)

project(hha_native LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "hha_native must be built with the Android NDK toolchain")
endif()

if(NOT "${ANDROID_ABI}" STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Only arm64-v8a is supported; got '${ANDROID_ABI}'")
endif()

set(HHA_SOURCES hha_native.cpp hha_bridge.cpp hha_genie.cpp hha_content.cpp)
set(HHA_TARGET hha_native)

add_library(${HHA_TARGET} SHARED ${HHA_SOURCES})

target_compile_features(${HHA_TARGET} PRIVATE cxx_std_17)
target_compile_options(${HHA_TARGET} PRIVATE
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

target_link_options(${HHA_TARGET} PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map"
    -Wl,--gc-sections
    -Wl,--no-undefined
    -Wl,-z,relro
    -Wl,-z,now
)

target_link_libraries(${HHA_TARGET} PRIVATE android log dl binder_ndk)

set_target_properties(${HHA_TARGET} PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "${HHA_TARGET}"
)

# Loaded only by the central system_server entry, never by target applications.
add_library(hha_content SHARED hha_content.cpp hha_genie.cpp)
target_compile_features(hha_content PRIVATE cxx_std_17)
target_compile_options(hha_content PRIVATE
    -fno-exceptions
    -fno-rtti
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -Wall
    -Wextra
    -Werror
)
target_link_options(hha_content PRIVATE
    -Wl,--no-undefined
    -Wl,-z,relro
    -Wl,-z,now
)
target_link_libraries(hha_content PRIVATE android log dl)
set_target_properties(hha_content PROPERTIES CXX_EXTENSIONS OFF)
