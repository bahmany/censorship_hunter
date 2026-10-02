# ─── Offline Country Database Support ───
# Offline IP geolocation database (HCGEO1 format, zstd compressed)
# Controlled by HUNTER_EMBED_GEO (ON/OFF, default OFF for offline CI).
option(HUNTER_EMBED_GEO "Embed DB-IP offline country database into the binary" OFF)
option(HUNTER_DOWNLOAD_GEO "Download fresh DB-IP country database at build time (requires network)" ON)
set(HUNTER_GEO_DB_PATH "${CMAKE_BINARY_DIR}/country.bin.zst" CACHE PATH "Path to country database to embed")
set(HUNTER_GEO_EMBED_OUT_DIR "${CMAKE_BINARY_DIR}/embedded_geo" CACHE PATH "Output dir for geo embed files")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(HUNTER_EMBED_GEO)
    if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(_geo_embed_platform "windows")
    else()
        set(_geo_embed_platform "linux")
    endif()

    if(NOT HUNTER_DOWNLOAD_GEO AND NOT EXISTS "${HUNTER_GEO_DB_PATH}")
        message(FATAL_ERROR
            "HUNTER_EMBED_GEO is ON but country database was not found at '${HUNTER_GEO_DB_PATH}'. "
            "Either enable HUNTER_DOWNLOAD_GEO=ON to fetch and build it automatically, "
            "or build it manually: ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_country_db.py --out ${HUNTER_GEO_DB_PATH}"
        )
    endif()

    if(HUNTER_DOWNLOAD_GEO)
        add_custom_command(
            OUTPUT ${HUNTER_GEO_DB_PATH}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}"
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_country_db.py
                    --out ${HUNTER_GEO_DB_PATH}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_country_db.py
            COMMENT "Downloading and building DB-IP HCGEO1 country database"
            VERBATIM
        )
    endif()

    add_custom_command(
        OUTPUT
            ${HUNTER_GEO_EMBED_OUT_DIR}/geo_embedded.h
            ${HUNTER_GEO_EMBED_OUT_DIR}/geo_embedded.S
            ${HUNTER_GEO_EMBED_OUT_DIR}/geo_meta.json
            ${HUNTER_GEO_EMBED_OUT_DIR}/country.zst
        COMMAND ${CMAKE_COMMAND} -E make_directory ${HUNTER_GEO_EMBED_OUT_DIR}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_country_db.py
                --embed-db ${HUNTER_GEO_DB_PATH}
                --embed-outdir ${HUNTER_GEO_EMBED_OUT_DIR}
                --platform ${_geo_embed_platform}
        DEPENDS ${HUNTER_GEO_DB_PATH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_country_db.py
        COMMENT "Embedding country database via zstd + .incbin"
        VERBATIM
    )

    set_source_files_properties(${HUNTER_GEO_EMBED_OUT_DIR}/geo_embedded.S
        PROPERTIES OBJECT_DEPENDS ${HUNTER_GEO_EMBED_OUT_DIR}/country.zst
    )

    add_library(geo_embedded_obj OBJECT ${HUNTER_GEO_EMBED_OUT_DIR}/geo_embedded.S)
    target_include_directories(geo_embedded_obj PRIVATE ${HUNTER_GEO_EMBED_OUT_DIR})

    target_sources(hunter_core PRIVATE $<TARGET_OBJECTS:geo_embedded_obj>)
    target_include_directories(hunter_core PUBLIC ${HUNTER_GEO_EMBED_OUT_DIR})
else()
    file(MAKE_DIRECTORY ${HUNTER_GEO_EMBED_OUT_DIR})
    file(WRITE ${HUNTER_GEO_EMBED_OUT_DIR}/geo_embedded.h
"// Stub — embedded geo disabled (HUNTER_EMBED_GEO=OFF).\n"
"#pragma once\n"
"#include <stdint.h>\n"
"#define HUNTER_EMBED_HAS_GEO 0\n"
"#define HUNTER_EMBED_GEO_USE_ZSTD 0\n"
)
    target_include_directories(hunter_core PUBLIC ${HUNTER_GEO_EMBED_OUT_DIR})
endif()

# Add country database source to hunter_core
target_sources(hunter_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/geo/country_database.cpp
)

# Test executable for offline country database
add_executable(test_geo
    tests/test_geo.cpp
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(test_geo PRIVATE -static-libgcc -static-libstdc++)
endif()
target_link_libraries(test_geo PRIVATE hunter_core)
add_test(NAME test_geo COMMAND test_geo)
