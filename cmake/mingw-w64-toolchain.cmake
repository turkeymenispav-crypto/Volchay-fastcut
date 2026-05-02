# Cross-compile toolchain file for building Windows binaries from Linux
# using MinGW-w64 (x86_64). Use with:
#
#   cmake -S . -B build -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-toolchain.cmake \
#         -DCMAKE_BUILD_TYPE=Release

set(CMAKE_SYSTEM_NAME      Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TRIPLE "x86_64-w64-mingw32" CACHE STRING "MinGW-w64 triple")

# Force the POSIX thread model. The "win32" thread model variant of
# mingw-w64 ships without std::mutex / std::thread / std::condition_variable
# support; we use those, so the toolchain explicitly selects the POSIX
# variant that has them.
set(CMAKE_C_COMPILER   ${MINGW_TRIPLE}-gcc-posix)
set(CMAKE_CXX_COMPILER ${MINGW_TRIPLE}-g++-posix)
set(CMAKE_RC_COMPILER  ${MINGW_TRIPLE}-windres)
set(CMAKE_AR           ${MINGW_TRIPLE}-ar)
set(CMAKE_RANLIB       ${MINGW_TRIPLE}-ranlib)
set(CMAKE_STRIP        ${MINGW_TRIPLE}-strip)

set(CMAKE_FIND_ROOT_PATH /usr/${MINGW_TRIPLE})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Static linking of the runtime so the produced .exe needs no extra DLLs
# beyond those that ship with Windows itself. This keeps cold start fast
# (no extra DLL probing) and the binary self-contained.
set(VOLCHAY_STATIC_RUNTIME_FLAGS "-static -static-libgcc -static-libstdc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${VOLCHAY_STATIC_RUNTIME_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${VOLCHAY_STATIC_RUNTIME_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${VOLCHAY_STATIC_RUNTIME_FLAGS}")
