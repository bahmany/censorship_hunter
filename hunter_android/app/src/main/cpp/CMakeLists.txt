cmake_minimum_required(VERSION 3.22.1)
project(hunter_android VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Paths (all relative to this file; no machine-specific locations)
get_filename_component(HUNTER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../hunter_cpp" ABSOLUTE)
get_filename_component(HUNTER_ANDROID_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
# Prebuilt static zlib/mbedTLS/libcurl produced by scripts/build_deps.sh
set(HUNTER_ANDROID_DEPS "${HUNTER_ANDROID_ROOT}/.deps" CACHE PATH "Dependency work dir")
set(CROSS_PREFIX ${HUNTER_ANDROID_DEPS}/install/${ANDROID_ABI})
if(NOT EXISTS ${CROSS_PREFIX}/lib/libcurl.a)
    message(FATAL_ERROR "Missing ${CROSS_PREFIX}/lib/libcurl.a - run hunter_android/scripts/build_deps.sh (Gradle does this automatically)")
endif()
set(IMGUI_DIR ${HUNTER_SRC_DIR}/third_party/imgui)
if(NOT EXISTS ${IMGUI_DIR}/imgui.cpp)
    # Submodule not checked out: fetch the exact pinned commit instead.
    include(FetchContent)
    FetchContent_Declare(imgui_src
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG d15966ff6cb48adacaae2f6d40230b4194d8ea70)
    FetchContent_MakeAvailable(imgui_src)
    set(IMGUI_DIR ${imgui_src_SOURCE_DIR})
endif()
set(QR_DIR ${HUNTER_SRC_DIR}/third_party/qrcodegen)

# Include directories
include_directories(
    ${HUNTER_SRC_DIR}/include
    ${HUNTER_SRC_DIR}
    ${CROSS_PREFIX}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${QR_DIR}
)

# ─── Hunter Core Library (same sources as desktop) ───
set(CORE_SOURCES
    ${HUNTER_SRC_DIR}/src/core/config.cpp
    ${HUNTER_SRC_DIR}/src/core/models.cpp
    ${HUNTER_SRC_DIR}/src/core/utils.cpp
    ${HUNTER_SRC_DIR}/src/core/task_manager.cpp
    ${HUNTER_SRC_DIR}/src/core/seed_data.cpp
    ${HUNTER_SRC_DIR}/src/core/engine_embed.cpp
    ${HUNTER_SRC_DIR}/src/core/updater.cpp
    ${HUNTER_SRC_DIR}/src/core/db_format.cpp
    ${HUNTER_SRC_DIR}/src/core/endpoint_key.cpp
    ${HUNTER_SRC_DIR}/src/core/health_score.cpp
)

# Vendored zstd decoder (for embedded engine decompression).
set(ZSTD_DIR ${HUNTER_SRC_DIR}/third_party/zstd)
set(ZSTD_SOURCES ${ZSTD_DIR}/zstddeclib.c)

# Android uses APK assets for engines (not .incbin embedding).
# Generate a stub header so engine_embed.cpp compiles — the runtime
# receives engine paths via nativeSetEngineDir() from Java.
set(EMBED_OUT_DIR ${CMAKE_BINARY_DIR}/embedded)
file(MAKE_DIRECTORY ${EMBED_OUT_DIR})
file(WRITE ${EMBED_OUT_DIR}/engine_embedded.h
"// Stub for Android — engines come from APK assets, not .incbin.\n"
"// Runtime receives engine paths via nativeSetEngineDir() from Java.\n\n"
"#pragma once\n"
"#define HUNTER_EMBED_HAS_ENGINES 0\n"
"#define HUNTER_EMBED_USE_ZSTD 0\n"
)
# Same stub treatment for the embedded geo DB — country_database.cpp
# guards on HUNTER_EMBED_HAS_GEO.
file(WRITE ${EMBED_OUT_DIR}/geo_embedded.h
"// Stub for Android — geo DB ships separately, not .incbin-embedded.\n"
"#pragma once\n"
"#include <stdint.h>\n"
"#define HUNTER_EMBED_HAS_GEO 0\n"
"#define HUNTER_EMBED_GEO_USE_ZSTD 0\n"
)
set(NETWORK_SOURCES
    ${HUNTER_SRC_DIR}/src/network/http_client.cpp
    ${HUNTER_SRC_DIR}/src/network/uri_parser.cpp
    ${HUNTER_SRC_DIR}/src/network/config_fetcher.cpp
    ${HUNTER_SRC_DIR}/src/network/continuous_validator.cpp
    ${HUNTER_SRC_DIR}/src/network/proxy_tester.cpp
    ${HUNTER_SRC_DIR}/src/network/aggressive_harvester.cpp
    ${HUNTER_SRC_DIR}/src/network/flexible_fetcher.cpp
    ${HUNTER_SRC_DIR}/src/network/sys_proxy.cpp
    ${HUNTER_SRC_DIR}/src/network/config_db_store.cpp
    ${HUNTER_SRC_DIR}/src/network/connectivity_baseline.cpp
    ${HUNTER_SRC_DIR}/src/network/engine_launcher.cpp
    ${HUNTER_SRC_DIR}/src/network/port_lease.cpp
    ${HUNTER_SRC_DIR}/src/network/traffic_probe.cpp
    ${HUNTER_SRC_DIR}/src/network/country_provider.cpp
)
set(GEO_SOURCES
    ${HUNTER_SRC_DIR}/src/geo/country_database.cpp
    ${HUNTER_SRC_DIR}/src/geo/country_query.cpp
    ${HUNTER_SRC_DIR}/src/geo/country_service.cpp
)
set(PROXY_SOURCES
    ${HUNTER_SRC_DIR}/src/proxy/xray_manager.cpp
    ${HUNTER_SRC_DIR}/src/proxy/proxy_server_manager.cpp
    ${HUNTER_SRC_DIR}/src/proxy/process_runner.cpp
    ${HUNTER_SRC_DIR}/src/proxy/health_monitor.cpp
)
set(CACHE_SOURCES
    ${HUNTER_SRC_DIR}/src/cache/smart_cache.cpp
)
set(ORCHESTRATOR_SOURCES
    ${HUNTER_SRC_DIR}/src/orchestrator/thread_manager.cpp
    ${HUNTER_SRC_DIR}/src/orchestrator/orchestrator.cpp
    ${HUNTER_SRC_DIR}/src/orchestrator/runtime_cleanup_manager.cpp
)

add_library(hunter_core STATIC
    ${CORE_SOURCES}
    ${NETWORK_SOURCES}
    ${GEO_SOURCES}
    ${PROXY_SOURCES}
    ${CACHE_SOURCES}
    ${ORCHESTRATOR_SOURCES}
    ${ZSTD_SOURCES}
)
target_compile_definitions(hunter_core PUBLIC CURL_STATICLIB)
target_include_directories(hunter_core PUBLIC ${ZSTD_DIR} ${EMBED_OUT_DIR})
target_link_libraries(hunter_core PUBLIC
    ${CROSS_PREFIX}/lib/libcurl.a
    ${CROSS_PREFIX}/lib/libmbedtls.a
    ${CROSS_PREFIX}/lib/libmbedx509.a
    ${CROSS_PREFIX}/lib/libmbedcrypto.a
    ${CROSS_PREFIX}/lib/libz.a
    log
    android
)

# ─── ImGui with OpenGL ES3 backend ───
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
set(QR_SOURCES
    ${QR_DIR}/qrcodegen.cpp
)

# ─── JNI bridge + Android platform backend + GUI ───
set(JNI_SOURCES
    jni_bridge.cpp
    imgui_impl_android_custom.cpp
    android_app.cpp
    config_embed_android.cpp
    vpn_jni.cpp
    ${HUNTER_SRC_DIR}/src/gui/app.cpp
)

set_source_files_properties(${HUNTER_SRC_DIR}/src/gui/app.cpp PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/android_compat_shim.h")

# ─── Native shared library ───
add_library(hunter SHARED
    ${JNI_SOURCES}
    ${IMGUI_SOURCES}
    ${QR_SOURCES}
)

target_link_libraries(hunter PRIVATE
    hunter_core
    EGL
    GLESv3
    log
    android
)

# Find Android log and app functions
find_library(log-lib log)
find_library(android-lib android)
