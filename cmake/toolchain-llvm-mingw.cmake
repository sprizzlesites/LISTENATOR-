# Cross-compile LISTENATOR to a Windows x86-64 VST3 from Linux using llvm-mingw.
#
# Plain GCC-mingw does not work: JUCE 9 only defines JUCE_64BIT under MSVC, so
# pointer_sized_int silently becomes 32-bit. llvm-mingw (Clang) plus the patches
# in patch-juce.cmake does work.
#
# Download llvm-mingw from https://github.com/mstorsjo/llvm-mingw/releases and
# point LLVM_MINGW at it (or set -DLLVM_MINGW=/path when configuring).

if(NOT DEFINED LLVM_MINGW)
    if(DEFINED ENV{LLVM_MINGW})
        set(LLVM_MINGW $ENV{LLVM_MINGW})
    else()
        file(GLOB _llvm_candidates /opt/llvm-mingw-*-ucrt-*)
        list(SORT _llvm_candidates)
        list(REVERSE _llvm_candidates)
        list(GET _llvm_candidates 0 LLVM_MINGW)
    endif()
endif()

if(NOT EXISTS "${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++")
    message(FATAL_ERROR "llvm-mingw not found at '${LLVM_MINGW}'. "
                        "Set -DLLVM_MINGW=/path/to/llvm-mingw-<ver>-ucrt-<host>.")
endif()

set(CMAKE_SYSTEM_NAME      Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER  ${LLVM_MINGW}/bin/x86_64-w64-mingw32-windres)
set(CMAKE_AR           ${LLVM_MINGW}/bin/llvm-ar)
set(CMAKE_RANLIB       ${LLVM_MINGW}/bin/llvm-ranlib)

set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW}/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# JUCE_64BIT     - JUCE only sets it for MSVC; without it pointers truncate.
# _WIN32_WINNT   - GetDpiForWindow and friends are gated behind Windows 10.
# NTDDI_VERSION  - must be >= NI, or d2d1_3.h hides ID2D1DeviceContext3.
# -include       - JUCE 9 relies on transitive <cstring>/<cstdint> that
#                  libstdc++/libc++ no longer provide. The shim pulls in only
#                  std headers: it must never include <windows.h>, which would
#                  precede JUCE's UNICODE setup and break every TCHAR type.
set(_ljp_defs "-DJUCE_64BIT=1 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A00000C")
set(CMAKE_CXX_FLAGS_INIT "${_ljp_defs} -include ${CMAKE_CURRENT_LIST_DIR}/mingw_compat.h")
set(CMAKE_C_FLAGS_INIT   "${_ljp_defs}")

# -static links libc++ and libunwind in. Without it the .vst3 imports
# libc++.dll / libunwind.dll, which no end user's Windows machine has.
#
# A VST3 is a CMake MODULE library, so MODULE_LINKER_FLAGS is the one that
# actually reaches the plugin's link line -- SHARED_LINKER_FLAGS does not.
set(_ljp_link "-static -static-libgcc -Wl,--allow-multiple-definition")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_ljp_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_ljp_link}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_ljp_link}")
