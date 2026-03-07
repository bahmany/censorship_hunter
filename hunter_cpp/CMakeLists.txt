cmake_minimum_required(VERSION 3.16)

project(hunter_cpp VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    # Avoid cc1plus out-of-memory on some machines by forcing single compile job.
    set_property(GLOBAL PROPERTY JOB_POOLS compile_pool=1)
    set(CMAKE_JOB_POOL_COMPILE compile_pool)
endif()

find_package(Threads REQUIRED)

# Find static zlib
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
find_package(ZLIB REQUIRED)

# Find libcurl
if(NOT WIN32)
    set(CURL_USE_STATIC_LIBS ON)
    find_package(CURL REQUIRED)
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR})

# ─── Core Library Sources ───
set(CORE_SOURCES
    src/core/config.cpp
    src/core/models.cpp
    src/core/utils.cpp
    src/core/task_manager.cpp
)

# ─── Network Library Sources ───
set(NETWORK_SOURCES
    src/network/http_client.cpp
    src/network/uri_parser.cpp
    src/network/config_fetcher.cpp
    src/network/continuous_validator.cpp
    src/network/proxy_tester.cpp
    src/network/aggressive_harvester.cpp
    src/network/flexible_fetcher.cpp
)

# ─── Proxy Library Sources ───
set(PROXY_SOURCES
    src/proxy/load_balancer.cpp
    src/proxy/xray_manager.cpp
)

# ─── Testing Library Sources ───
set(TESTING_SOURCES
    src/testing/benchmark.cpp
)

# ─── Security Library Sources ───
set(SECURITY_SOURCES
    src/security/dpi_evasion.cpp
    src/security/obfuscation.cpp
)

# ─── Telegram Library Sources ───
set(TELEGRAM_SOURCES
    src/telegram/bot_reporter.cpp
)

# ─── Cache Library Sources ───
set(CACHE_SOURCES
    src/cache/smart_cache.cpp
)

# ─── Orchestrator Library Sources ───
set(ORCHESTRATOR_SOURCES
    src/orchestrator/thread_manager.cpp
    src/orchestrator/orchestrator.cpp
)

# ─── Web Dashboard Sources ───
# Create hunter core static library
add_library(hunter_core STATIC
    ${CORE_SOURCES}
    ${NETWORK_SOURCES}
    ${PROXY_SOURCES}
    ${TESTING_SOURCES}
    ${SECURITY_SOURCES}
    ${TELEGRAM_SOURCES}
    ${CACHE_SOURCES}
    ${ORCHESTRATOR_SOURCES}
)

target_link_libraries(hunter_core
    PUBLIC
        Threads::Threads
        ${ZLIB_LIBRARIES}
)

target_include_directories(hunter_core PUBLIC
    ${ZLIB_INCLUDE_DIRS}
)

if(WIN32)
    # Use MSYS2 UCRT static libcurl to avoid shipping DLLs
    get_filename_component(_msys_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_msys_lib_dir "${_msys_bin_dir}/../lib")
    target_include_directories(hunter_core PUBLIC "${_msys_bin_dir}/../include")
    target_link_libraries(hunter_core PUBLIC
        "${_msys_lib_dir}/libcurl.a"
        "${_msys_lib_dir}/libssl.a"
        "${_msys_lib_dir}/libcrypto.a"
        "${_msys_lib_dir}/libssh2.a"
        "${_msys_lib_dir}/libnghttp2.a"
        "${_msys_lib_dir}/libnghttp3.a"
        "${_msys_lib_dir}/libpsl.a"
        "${_msys_lib_dir}/libidn2.a"
        "${_msys_lib_dir}/libunistring.a"
        "${_msys_lib_dir}/libiconv.a"
        "${_msys_lib_dir}/libbrotlidec.a"
        "${_msys_lib_dir}/libbrotlicommon.a"
        "${_msys_lib_dir}/libzstd.a"
        "${_msys_lib_dir}/libz.a"
        wldap32
        bcrypt
        advapi32
        crypt32
        gdi32
        ws2_32
        winhttp
    )
    target_compile_definitions(hunter_core PUBLIC CURL_STATICLIB)
else()
    target_link_libraries(hunter_core PUBLIC CURL::libcurl)
    target_include_directories(hunter_core PUBLIC ${CURL_INCLUDE_DIRS})
endif()

# Static link C++ runtime
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static-libgcc -static-libstdc++")

# Unicode support
add_definitions(-DUNICODE -D_UNICODE)

# Console CLI executable
add_executable(hunter_cli
    src/main.cpp
)
target_link_options(hunter_cli PRIVATE -static)
target_link_libraries(hunter_cli PRIVATE hunter_core)

add_custom_command(TARGET hunter_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/../bin"
    COMMAND powershell -NoProfile -Command "Copy-Item -Force -ErrorAction SilentlyContinue -LiteralPath '$<TARGET_FILE:hunter_cli>' -Destination '${CMAKE_SOURCE_DIR}/../bin/$<TARGET_FILE_NAME:hunter_cli>'"
)

# Windows GUI executable (Windows only)
if(WIN32)
    add_executable(hunter_ui
        src/win32/win_main.cpp
        src/win32/main_window.cpp
        src/win32/resource.rc
    )
    target_link_options(hunter_ui PRIVATE -static -mwindows -municode)
    target_link_libraries(hunter_ui PRIVATE 
        hunter_core 
        comctl32 
        shell32 
        dwmapi 
        uxtheme
    )
    set_target_properties(hunter_ui PROPERTIES
        WIN32_EXECUTABLE TRUE
    )

    set(_hunter_deploy_dir "${CMAKE_SOURCE_DIR}/../bin")

    add_custom_command(TARGET hunter_ui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_hunter_deploy_dir}"
        COMMAND powershell -NoProfile -Command "Copy-Item -Force -ErrorAction SilentlyContinue -LiteralPath '$<TARGET_FILE:hunter_ui>' -Destination '${_hunter_deploy_dir}/$<TARGET_FILE_NAME:hunter_ui>'"
    )

endif()

# Install targets
install(TARGETS hunter_core hunter_cli
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include
    PATTERN "web" EXCLUDE
)
