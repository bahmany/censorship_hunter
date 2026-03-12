set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(MSVC_DIR "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207")
set(SDK_DIR "C:/Program Files (x86)/Windows Kits/10")
set(SDK_VER "10.0.26100.0")

set(CMAKE_C_COMPILER "${MSVC_DIR}/bin/Hostx64/x64/cl.exe")
set(CMAKE_CXX_COMPILER "${MSVC_DIR}/bin/Hostx64/x64/cl.exe")
set(CMAKE_LINKER "${MSVC_DIR}/bin/Hostx64/x64/link.exe")
set(CMAKE_AR "${MSVC_DIR}/bin/Hostx64/x64/lib.exe")
set(CMAKE_RC_COMPILER "${SDK_DIR}/bin/${SDK_VER}/x64/rc.exe")
set(CMAKE_MT "${SDK_DIR}/bin/${SDK_VER}/x64/mt.exe")

include_directories(SYSTEM
  "${MSVC_DIR}/include"
  "${SDK_DIR}/Include/${SDK_VER}/ucrt"
  "${SDK_DIR}/Include/${SDK_VER}/um"
  "${SDK_DIR}/Include/${SDK_VER}/shared"
  "${SDK_DIR}/Include/${SDK_VER}/winrt"
  "${SDK_DIR}/Include/${SDK_VER}/cppwinrt"
)

link_directories(
  "${MSVC_DIR}/lib/x64"
  "${SDK_DIR}/Lib/${SDK_VER}/ucrt/x64"
  "${SDK_DIR}/Lib/${SDK_VER}/um/x64"
)

set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
  "${MSVC_DIR}/include"
  "${SDK_DIR}/Include/${SDK_VER}/ucrt"
  "${SDK_DIR}/Include/${SDK_VER}/um"
  "${SDK_DIR}/Include/${SDK_VER}/shared"
  "${SDK_DIR}/Include/${SDK_VER}/winrt"
  "${SDK_DIR}/Include/${SDK_VER}/cppwinrt"
)

set(ENV{INCLUDE} "${MSVC_DIR}/include;${SDK_DIR}/Include/${SDK_VER}/ucrt;${SDK_DIR}/Include/${SDK_VER}/um;${SDK_DIR}/Include/${SDK_VER}/shared")
set(ENV{LIB} "${MSVC_DIR}/lib/x64;${SDK_DIR}/Lib/${SDK_VER}/ucrt/x64;${SDK_DIR}/Lib/${SDK_VER}/um/x64")
set(ENV{PATH} "${MSVC_DIR}/bin/Hostx64/x64;${SDK_DIR}/bin/${SDK_VER}/x64;$ENV{PATH}")

# Disable CMake's RC dependency scanning which breaks with Ninja+MSVC rc.exe
set(CMAKE_DEPENDS_USE_COMPILER FALSE)
set(CMAKE_RC_FLAGS "/nologo")
set(CMAKE_NINJA_CMCLDEPS_RC OFF)
set(CMAKE_RC_COMPILER_INIT "${SDK_DIR}/bin/${SDK_VER}/x64/rc.exe")
