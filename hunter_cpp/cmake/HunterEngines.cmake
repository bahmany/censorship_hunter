# HunterEngines.cmake — reproducible, checksummed fetch of the proxy engines
# (xray-core, sing-box) so HUNTER_EMBED_ENGINES works from a clean checkout.
#
#   include(HunterEngines)
#   hunter_fetch_engines(PLATFORM windows|linux OUT_DIR <dir>)
#
# Versions and SHA256 digests are pinned below (digests are the ones published by
# the GitHub release API for each asset). To bump: change version + hashes together.
# Every download is verified (EXPECTED_HASH); a mismatch aborts configuration.
# Offline/air-gapped: pre-populate OUT_DIR with xray[.exe] and sing-box[.exe].

set(HUNTER_XRAY_VERSION     "v26.3.27")
set(HUNTER_SINGBOX_VERSION  "1.14.2")

set(HUNTER_XRAY_URL_BASE "https://github.com/XTLS/Xray-core/releases/download/${HUNTER_XRAY_VERSION}")
set(HUNTER_SINGBOX_URL_BASE "https://github.com/SagerNet/sing-box/releases/download/v${HUNTER_SINGBOX_VERSION}")

# asset names
set(HUNTER_XRAY_ASSET_windows    "Xray-windows-64.zip")
set(HUNTER_XRAY_ASSET_linux      "Xray-linux-64.zip")
set(HUNTER_SINGBOX_ASSET_windows "sing-box-${HUNTER_SINGBOX_VERSION}-windows-amd64.zip")
set(HUNTER_SINGBOX_ASSET_linux   "sing-box-${HUNTER_SINGBOX_VERSION}-linux-amd64.tar.gz")

# SHA256 of each release asset
set(HUNTER_XRAY_SHA256_windows    "d004c39288ce9ada487c6f398c7c545f7d749e44bdfdd59dbc9f865afba4e1ad")
set(HUNTER_XRAY_SHA256_linux      "23cd9af937744d97776ee35ecad4972cf4b2109d1e0fe6be9930467608f7c8ae")
set(HUNTER_SINGBOX_SHA256_windows "c2d8bfff918755808781dfdeeb8581b6c91eb3a243d9a7b55483cfc0c0684d32")
set(HUNTER_SINGBOX_SHA256_linux   "a684484d7477d1437282ee411f4d131d0340aaad60a7868841ebd5d87dd8a0c6")

function(_hunter_fetch_one NAME PLATFORM EXE_NAME OUT_DIR)
    string(TOUPPER "${NAME}" _N)
    set(_asset  "${HUNTER_${_N}_ASSET_${PLATFORM}}")
    set(_sha    "${HUNTER_${_N}_SHA256_${PLATFORM}}")
    set(_url    "${HUNTER_${_N}_URL_BASE}/${_asset}")
    set(_final  "${OUT_DIR}/${EXE_NAME}")
    set(_stamp  "${OUT_DIR}/.${_N}.${_sha}.ok")
    if(EXISTS "${_final}" AND EXISTS "${_stamp}")
        return()
    endif()
    if(EXISTS "${_final}" AND NOT EXISTS "${_stamp}")
        message(STATUS "Using user-provided ${EXE_NAME} in ${OUT_DIR} (unverified)")
        return()
    endif()
    set(_dl "${OUT_DIR}/_dl")
    file(MAKE_DIRECTORY "${_dl}")
    message(STATUS "Fetching ${_asset} (sha256 ${_sha})")
    file(DOWNLOAD "${_url}" "${_dl}/${_asset}" EXPECTED_HASH SHA256=${_sha}
         TLS_VERIFY ON SHOW_PROGRESS STATUS _st)
    list(GET _st 0 _rc)
    if(NOT _rc EQUAL 0)
        file(REMOVE "${_dl}/${_asset}")
        message(FATAL_ERROR "Download/verify failed for ${_url}: ${_st}")
    endif()
    set(_x "${_dl}/${NAME}_x")
    file(REMOVE_RECURSE "${_x}")
    file(MAKE_DIRECTORY "${_x}")
    file(ARCHIVE_EXTRACT INPUT "${_dl}/${_asset}" DESTINATION "${_x}")
    file(GLOB_RECURSE _hit "${_x}/${EXE_NAME}")
    list(LENGTH _hit _n)
    if(_n EQUAL 0)
        message(FATAL_ERROR "${EXE_NAME} not found inside ${_asset}")
    endif()
    list(GET _hit 0 _src)
    file(COPY_FILE "${_src}" "${_final}")
    file(CHMOD "${_final}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    file(WRITE "${_stamp}" "${_url}\n${_sha}\n")
    file(REMOVE_RECURSE "${_x}")
    file(REMOVE "${_dl}/${_asset}")
endfunction()

function(hunter_fetch_engines)
    cmake_parse_arguments(A "" "PLATFORM;OUT_DIR" "" ${ARGN})
    if(A_PLATFORM STREQUAL "windows")
        set(_x "xray.exe")
        set(_s "sing-box.exe")
    else()
        set(_x "xray")
        set(_s "sing-box")
    endif()
    file(MAKE_DIRECTORY "${A_OUT_DIR}")
    _hunter_fetch_one(XRAY     ${A_PLATFORM} ${_x} "${A_OUT_DIR}")
    _hunter_fetch_one(SINGBOX ${A_PLATFORM} ${_s} "${A_OUT_DIR}")
endfunction()
