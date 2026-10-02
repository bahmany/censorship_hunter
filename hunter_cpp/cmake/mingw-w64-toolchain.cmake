# CMake toolchain for MinGW-w64 cross-compilation (Linux -> Windows)
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Cross-compiled dependencies install prefix
set(CROSS_PREFIX /home/mohammad/Documents/projects/censorship_hunter/cross_build/mingw/install)
list(APPEND CMAKE_PREFIX_PATH ${CROSS_PREFIX})

# Static linking flags
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++ -lwinmm -lgdi32 -lws2_32 -ladvapi32 -luser32 -lshell32 -lole32 -loleaut32 -luuid -limm32 -lcomdlg32 -lsetupapi -lntdll")
