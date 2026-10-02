# CMake toolchain file for MinGW-w64 cross-compilation (Linux → Windows x64)
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CROSS_PREFIX /home/mohammad/Documents/projects/censorship_hunter/cross_build/mingw/install)

set(ZLIB_INCLUDE_DIR ${CROSS_PREFIX}/include)
set(ZLIB_LIBRARY ${CROSS_PREFIX}/lib/libzlibstatic.a)
set(ZLIB_LIBRARIES ${ZLIB_LIBRARY})

set(CURL_INCLUDE_DIR ${CROSS_PREFIX}/include)
set(CURL_LIBRARY ${CROSS_PREFIX}/lib/libcurl.a)

set(glfw3_DIR ${CROSS_PREFIX}/lib/cmake/glfw3)

set(CMAKE_FIND_ROOT_PATH ${CROSS_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
