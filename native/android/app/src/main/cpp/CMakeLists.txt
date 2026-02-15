cmake_minimum_required(VERSION 3.22.1)
project("hunter" VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Fetch nlohmann/json (header-only)
include(FetchContent)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(json)

# Source files
set(HUNTER_SOURCES
    core/config.cpp
    core/utils.cpp
    parsers/uri_parser.cpp
    network/http_client.cpp
    testing/benchmark.cpp
    proxy/load_balancer.cpp
    telegram/scraper.cpp
    security/obfuscation.cpp
    cache/cache.cpp
    orchestrator.cpp
    hunter_jni.cpp
)

# Build shared library
add_library(hunter SHARED ${HUNTER_SOURCES})

# Include directories
target_include_directories(hunter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/parsers
    ${CMAKE_CURRENT_SOURCE_DIR}/network
    ${CMAKE_CURRENT_SOURCE_DIR}/testing
    ${CMAKE_CURRENT_SOURCE_DIR}/proxy
    ${CMAKE_CURRENT_SOURCE_DIR}/telegram
    ${CMAKE_CURRENT_SOURCE_DIR}/security
    ${CMAKE_CURRENT_SOURCE_DIR}/cache
)

# Link libraries
find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(hunter
    nlohmann_json::nlohmann_json
    ${log-lib}
    ${android-lib}
)

# Compiler warnings
target_compile_options(hunter PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -fvisibility=hidden
)

# Strip in release
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(hunter PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(hunter PRIVATE -Wl,--gc-sections -Wl,--strip-all)
endif()
