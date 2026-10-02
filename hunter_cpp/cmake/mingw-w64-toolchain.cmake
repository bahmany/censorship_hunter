# CMake toolchain: MinGW-w64 cross-compile (Linux/macOS host -> Windows x86_64).
# Toolchain: apt install mingw-w64 (use the *-posix thread model: std::thread needs it)
#            or MSYS2/brew equivalent. Override with -DHUNTER_MINGW_TRIPLE=... if needed.
# Static deps prefix (zlib/curl/glfw): built by scripts/fetch_windows_deps.sh, located via
#   -DHUNTER_WIN_DEPS=<dir>  or env HUNTER_WIN_DEPS  (default: <repo>/build/win-deps).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT HUNTER_MINGW_TRIPLE)
    set(HUNTER_MINGW_TRIPLE x86_64-w64-mingw32)
endif()

# Prefer the posix-threads flavour (Debian/Ubuntu ship both win32 and posix).
find_program(_hunter_cc  NAMES ${HUNTER_MINGW_TRIPLE}-gcc-posix ${HUNTER_MINGW_TRIPLE}-gcc REQUIRED)
find_program(_hunter_cxx NAMES ${HUNTER_MINGW_TRIPLE}-g++-posix ${HUNTER_MINGW_TRIPLE}-g++ REQUIRED)
find_program(_hunter_rc  NAMES ${HUNTER_MINGW_TRIPLE}-windres REQUIRED)
set(CMAKE_C_COMPILER   ${_hunter_cc})
set(CMAKE_CXX_COMPILER ${_hunter_cxx})
set(CMAKE_RC_COMPILER  ${_hunter_rc})
set(CMAKE_ASM_COMPILER ${_hunter_cc})

if(NOT HUNTER_WIN_DEPS AND DEFINED ENV{HUNTER_WIN_DEPS})
    set(HUNTER_WIN_DEPS "$ENV{HUNTER_WIN_DEPS}")
endif()
if(NOT HUNTER_WIN_DEPS)
    get_filename_component(HUNTER_WIN_DEPS "${CMAKE_CURRENT_LIST_DIR}/../../build/win-deps" ABSOLUTE)
endif()
set(HUNTER_WIN_DEPS "${HUNTER_WIN_DEPS}" CACHE PATH "Prefix with static zlib/curl/glfw for Windows")

execute_process(COMMAND ${_hunter_cc} -print-sysroot OUTPUT_VARIABLE _sysroot OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(CMAKE_FIND_ROOT_PATH ${HUNTER_WIN_DEPS} /usr/${HUNTER_MINGW_TRIPLE} ${_sysroot})
list(APPEND CMAKE_PREFIX_PATH ${HUNTER_WIN_DEPS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
