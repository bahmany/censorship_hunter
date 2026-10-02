cmake_minimum_required(VERSION 3.16)

project(hunter_cpp VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable assembly language for .incbin-based engine embedding.
enable_language(ASM)

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenGL REQUIRED)

include(FindPkgConfig)
pkg_check_modules(GLFW REQUIRED glfw3)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR})

# ─── Embedded Engine Support ───
# The single-file build compresses xray + sing-box with zstd and embeds them
# via .incbin (assembler). This is controlled by HUNTER_EMBED_ENGINES (ON/OFF).
# When ON, embed_engines.py runs at build time to generate:
#   ${CMAKE_BINARY_DIR}/embedded/engine_embedded.h  (declarations + metadata)
#   ${CMAKE_BINARY_DIR}/embedded/engine_embedded.S  (.incbin blob)
#   ${CMAKE_BINARY_DIR}/embedded/*.zst              (compressed payloads)
# When OFF or bin/ has no engines, a stub header is generated and the binary
# falls back to classic bin/xray, bin/sing-box lookup at runtime.
option(HUNTER_EMBED_ENGINES "Embed xray/sing-box into the binary (single-file mode)" OFF)
set(HUNTER_EMBED_BIN_DIR "${CMAKE_SOURCE_DIR}/../bin" CACHE PATH "Directory containing engine binaries to embed")
set(HUNTER_EMBED_OUT_DIR "${CMAKE_BINARY_DIR}/embedded" CACHE PATH "Output dir for generated embed files")

if(HUNTER_EMBED_ENGINES)
    # Determine target platform for .S section directives.
    if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(_embed_platform "windows")
    else()
        set(_embed_platform "linux")
    endif()

    # Run embed_engines.py to generate header + .S + compressed blobs.
    add_custom_command(
        OUTPUT
            ${HUNTER_EMBED_OUT_DIR}/engine_embedded.h
            ${HUNTER_EMBED_OUT_DIR}/engine_embedded.S
            ${HUNTER_EMBED_OUT_DIR}/engine_meta.json
        COMMAND ${CMAKE_COMMAND} -E make_directory ${HUNTER_EMBED_OUT_DIR}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/embed_engines.py
                --bin ${HUNTER_EMBED_BIN_DIR}
                --outdir ${HUNTER_EMBED_OUT_DIR}
                --platform ${_embed_platform}
        DEPENDS ${HUNTER_EMBED_BIN_DIR}/xray ${HUNTER_EMBED_BIN_DIR}/sing-box
                ${CMAKE_SOURCE_DIR}/tools/embed_engines.py
        COMMENT "Embedding proxy engines (xray, sing-box) via zstd + .incbin"
        VERBATIM
    )

    # Compile the generated .S (assembler with .incbin) into an object library.
    add_library(engine_embedded_obj OBJECT ${HUNTER_EMBED_OUT_DIR}/engine_embedded.S)
    target_include_directories(engine_embedded_obj PRIVATE ${HUNTER_EMBED_OUT_DIR})

    # Propagate the generated header include dir to consumers.
    set(HUNTER_EMBED_INCLUDE_DIR ${HUNTER_EMBED_OUT_DIR})
    set(HUNTER_EMBED_OBJECT $<TARGET_OBJECTS:engine_embedded_obj>)
else()
    # Stub: no embedded engines — runtime falls back to bin/ lookup.
    # Generate a minimal header so engine_embed.cpp compiles without payloads.
    file(MAKE_DIRECTORY ${HUNTER_EMBED_OUT_DIR})
    file(WRITE ${HUNTER_EMBED_OUT_DIR}/engine_embedded.h
"// Stub — embedded engines disabled (HUNTER_EMBED_ENGINES=OFF).\n"
"// Runtime falls back to bin/xray, bin/sing-box lookup.\n\n"
"#pragma once\n"
"#define HUNTER_EMBED_HAS_ENGINES 0\n"
"#define HUNTER_EMBED_USE_ZSTD 0\n"
)
    set(HUNTER_EMBED_INCLUDE_DIR ${HUNTER_EMBED_OUT_DIR})
    set(HUNTER_EMBED_OBJECT "")
endif()

# ─── Embedded Configs Support ───
# The single-file build downloads the latest proxy configs from 25+ public
# GitHub sources at BUILD TIME, compresses the deduplicated bundle with zstd,
# and embeds it via .incbin so the binary ships with a full working config
# set on first launch. Controlled by HUNTER_EMBED_CONFIGS (ON/OFF).
option(HUNTER_EMBED_CONFIGS "Download + embed proxy configs into the binary (single-file mode)" OFF)
option(HUNTER_DOWNLOAD_CONFIGS "Download fresh configs at build time (requires network)" ON)
set(HUNTER_CONFIG_BUNDLE "${CMAKE_BINARY_DIR}/configs_bundle.txt" CACHE PATH "Path to config bundle to embed")
set(HUNTER_CONFIG_EMBED_OUT_DIR "${CMAKE_BINARY_DIR}/embedded_configs" CACHE PATH "Output dir for config embed files")
set(HUNTER_CONFIG_DOWNLOAD_TIMEOUT "30" CACHE STRING "Per-source download timeout (seconds)")
set(HUNTER_CONFIG_DOWNLOAD_WORKERS "16" CACHE STRING "Parallel download workers")
set(HUNTER_CONFIG_MIN_CONFIGS "100" CACHE STRING "Minimum configs required for a successful build")

if(HUNTER_EMBED_CONFIGS)
    # Determine target platform for .S section directives.
    if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(_cfg_embed_platform "windows")
    else()
        set(_cfg_embed_platform "linux")
    endif()

    # Step 1: Download fresh configs (unless HUNTER_DOWNLOAD_CONFIGS=OFF or
    # a pre-built bundle is supplied via HUNTER_CONFIG_BUNDLE).
    if(HUNTER_DOWNLOAD_CONFIGS)
        add_custom_command(
            OUTPUT ${HUNTER_CONFIG_BUNDLE}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}"
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/download_configs.py
                    --outfile ${HUNTER_CONFIG_BUNDLE}
                    --timeout ${HUNTER_CONFIG_DOWNLOAD_TIMEOUT}
                    --workers ${HUNTER_CONFIG_DOWNLOAD_WORKERS}
                    --min-configs ${HUNTER_CONFIG_MIN_CONFIGS}
            DEPENDS ${CMAKE_SOURCE_DIR}/tools/download_configs.py
            COMMENT "Downloading latest proxy configs from 25+ public sources"
            VERBATIM
        )
    endif()

    # Step 2: Compress + generate .incbin assembler + header.
    add_custom_command(
        OUTPUT
            ${HUNTER_CONFIG_EMBED_OUT_DIR}/config_embedded.h
            ${HUNTER_CONFIG_EMBED_OUT_DIR}/config_embedded.S
            ${HUNTER_CONFIG_EMBED_OUT_DIR}/config_meta.json
        COMMAND ${CMAKE_COMMAND} -E make_directory ${HUNTER_CONFIG_EMBED_OUT_DIR}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/embed_configs.py
                --bundle ${HUNTER_CONFIG_BUNDLE}
                --outdir ${HUNTER_CONFIG_EMBED_OUT_DIR}
                --platform ${_cfg_embed_platform}
        DEPENDS ${HUNTER_CONFIG_BUNDLE}
                ${CMAKE_SOURCE_DIR}/tools/embed_configs.py
        COMMENT "Embedding config bundle via zstd + .incbin"
        VERBATIM
    )

    # Compile the generated .S into an object library.
    add_library(config_embedded_obj OBJECT ${HUNTER_CONFIG_EMBED_OUT_DIR}/config_embedded.S)
    target_include_directories(config_embedded_obj PRIVATE ${HUNTER_CONFIG_EMBED_OUT_DIR})

    set(HUNTER_CONFIG_EMBED_INCLUDE_DIR ${HUNTER_CONFIG_EMBED_OUT_DIR})
    set(HUNTER_CONFIG_EMBED_OBJECT $<TARGET_OBJECTS:config_embedded_obj>)
else()
    # Stub: no embedded configs — runtime falls back to live scraping.
    file(MAKE_DIRECTORY ${HUNTER_CONFIG_EMBED_OUT_DIR})
    file(WRITE ${HUNTER_CONFIG_EMBED_OUT_DIR}/config_embedded.h
"// Stub — embedded configs disabled (HUNTER_EMBED_CONFIGS=OFF).\n"
"// Runtime falls back to live scraping.\n\n"
"#pragma once\n"
"#define HUNTER_EMBED_HAS_CONFIGS 0\n"
"#define HUNTER_EMBED_CONFIGS_USE_ZSTD 0\n"
)
    set(HUNTER_CONFIG_EMBED_INCLUDE_DIR ${HUNTER_CONFIG_EMBED_OUT_DIR})
    set(HUNTER_CONFIG_EMBED_OBJECT "")
endif()

# ─── Vendored zstd decoder (amalgamated, decompress-only) ───
set(ZSTD_DIR ${CMAKE_SOURCE_DIR}/third_party/zstd)
set(ZSTD_SOURCES ${ZSTD_DIR}/zstddeclib.c)

# ─── Core Library Sources ───
set(CORE_SOURCES
    src/core/config.cpp
    src/core/models.cpp
    src/core/endpoint_key.cpp
    src/core/health_score.cpp
    src/core/db_format.cpp
    src/core/utils.cpp
    src/core/task_manager.cpp
    src/core/seed_data.cpp
    src/core/engine_embed.cpp
    src/core/config_embed.cpp
    src/core/updater.cpp
)

# ─── Network Library Sources ───
set(NETWORK_SOURCES
    src/network/http_client.cpp
    src/network/uri_parser.cpp
    src/network/config_fetcher.cpp
    src/network/continuous_validator.cpp
    src/network/config_db_store.cpp
    src/network/proxy_tester.cpp
    src/network/aggressive_harvester.cpp
    src/network/flexible_fetcher.cpp
    src/network/sys_proxy.cpp
)

# ─── Proxy Library Sources ───
# Only XRayManager survives — it provides the static config-generation
# helpers ProxyTester uses to spawn temporary test processes. Everything
# else in proxy/ (load balancer, gateway, session router, traffic
# observer, upstream tunnels, generated-config hosting) served live
# client traffic and has been removed.
set(PROXY_SOURCES
    src/proxy/xray_manager.cpp
    src/proxy/proxy_server_manager.cpp
)

# ─── Cache Library Sources ───
set(CACHE_SOURCES
    src/cache/smart_cache.cpp
)

# ─── Orchestrator Library Sources ───
set(ORCHESTRATOR_SOURCES
    src/orchestrator/thread_manager.cpp
    src/orchestrator/orchestrator.cpp
    src/orchestrator/runtime_cleanup_manager.cpp
)

# Create hunter core static library
add_library(hunter_core STATIC
    ${CORE_SOURCES}
    ${NETWORK_SOURCES}
    ${PROXY_SOURCES}
    ${CACHE_SOURCES}
    ${ORCHESTRATOR_SOURCES}
    ${ZSTD_SOURCES}
    ${HUNTER_EMBED_OBJECT}
    ${HUNTER_CONFIG_EMBED_OBJECT}
)

target_link_libraries(hunter_core
    PUBLIC
        Threads::Threads
        ${ZLIB_LIBRARIES}
        CURL::libcurl
)

target_include_directories(hunter_core PUBLIC
    ${ZLIB_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${ZSTD_DIR}
)

# Add embedded header include dir (always — stub or real, engine_embed.cpp needs it).
target_include_directories(hunter_core PUBLIC ${HUNTER_EMBED_INCLUDE_DIR})

# Add embedded config header include dir (always — stub or real, config_embed.cpp needs it).
target_include_directories(hunter_core PUBLIC ${HUNTER_CONFIG_EMBED_INCLUDE_DIR})

# Static link C++ runtime
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static-libgcc -static-libstdc++")

# ─── Dear ImGui (vendored) + GLFW/OpenGL2 backend ───
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp
)

# ─── QR Code generator (vendored, Nayuki public-domain) ───
set(QR_DIR ${CMAKE_SOURCE_DIR}/third_party/qrcodegen)
set(QR_SOURCES
    ${QR_DIR}/qrcodegen.cpp
)

# ─── GUI executable ───
add_executable(hunter
    src/gui/main.cpp
    src/gui/app.cpp
    src/gui/single_instance.cpp
    ${IMGUI_SOURCES}
    ${QR_SOURCES}
)

# ─── Application icon ───
# Embed the icon + version info into the binary on Windows via resources.rc.
# On Linux/macOS, the icon is set at runtime via glfwSetWindowIcon() (see
# app.cpp) and a .desktop file is installed for desktop integration.
set(HUNTER_ICON_PNG "${CMAKE_SOURCE_DIR}/hunter_icon.png")
set(HUNTER_ICON_ICO "${CMAKE_SOURCE_DIR}/hunter.ico")
if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # MSVC / MinGW resource file for Windows (.ico + version info + manifest)
    set(HUNTER_RC_FILE "${CMAKE_SOURCE_DIR}/resources.rc")
    target_sources(hunter PRIVATE ${HUNTER_RC_FILE})
endif()

target_include_directories(hunter PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${QR_DIR}
    ${GLFW_INCLUDE_DIRS}
)
target_link_libraries(hunter PRIVATE
    hunter_core
    ${GLFW_LIBRARIES}
    OpenGL::GL
)
target_link_options(hunter PRIVATE -static-libgcc -static-libstdc++ ${GLFW_LDFLAGS})

# Copy the PNG icon next to the binary so glfwSetWindowIcon() can load it
# at runtime on Linux (the icon is also embedded in the .ico/.rc on Windows).
add_custom_command(TARGET hunter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HUNTER_ICON_PNG}" "$<TARGET_FILE_DIR:hunter>/hunter_icon.png"
)

set(_hunter_deploy_dir "${CMAKE_SOURCE_DIR}/../bin")

add_custom_command(TARGET hunter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_hunter_deploy_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:hunter>" "${_hunter_deploy_dir}/hunter"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HUNTER_ICON_PNG}" "${_hunter_deploy_dir}/hunter_icon.png"
)

# Test executable
add_executable(hunter_tests
    tests/test_core.cpp
)
target_link_options(hunter_tests PRIVATE -static-libgcc -static-libstdc++)
target_link_libraries(hunter_tests PRIVATE hunter_core)

# Engine embed test executable
add_executable(test_embed
    tests/test_embed.cpp
)
target_link_options(test_embed PRIVATE -static-libgcc -static-libstdc++)
target_link_libraries(test_embed PRIVATE hunter_core)

# Stage 4 A1 unit suites (pure logic, no network)
foreach(_t test_endpoint_key test_health_score test_config_db_v4)
    add_executable(${_t} tests/${_t}.cpp)
    target_link_options(${_t} PRIVATE -static-libgcc -static-libstdc++)
    target_link_libraries(${_t} PRIVATE hunter_core)
endforeach()

# CTest registration
enable_testing()
add_test(NAME hunter_tests COMMAND hunter_tests)
add_test(NAME test_endpoint_key COMMAND test_endpoint_key)
add_test(NAME test_health_score COMMAND test_health_score)
add_test(NAME test_config_db_v4 COMMAND test_config_db_v4)
add_test(NAME test_embed COMMAND test_embed)

# Install targets
install(TARGETS hunter
    RUNTIME DESTINATION bin
)
install(TARGETS hunter_core
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/ DESTINATION include)

# ─── Linux desktop integration ───
# Install the .desktop file and icon so the app appears in application menus
# with the proper icon on Linux desktop environments (GNOME, KDE, etc.).
if(UNIX AND NOT APPLE)
    install(FILES "${CMAKE_SOURCE_DIR}/hunter.desktop"
            DESTINATION share/applications)
    install(FILES "${HUNTER_ICON_PNG}"
            DESTINATION share/icons/hicolor/256x256/apps
            RENAME hunter.png)
    # Also install a 48x48 icon for smaller displays.
    install(FILES "${HUNTER_ICON_PNG}"
            DESTINATION share/icons/hicolor/48x48/apps
            RENAME hunter.png)
endif()
