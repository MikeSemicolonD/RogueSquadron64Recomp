# Cross toolchain for 32-bit x86 Linux from an x86-64 Ubuntu host with i386 multiarch packages.
# SSE math (not x87) keeps float results identical to the 64-bit builds; SSE4.1 is the RSP vector path's floor.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_LIBRARY_ARCHITECTURE i386-linux-gnu)

set(CMAKE_C_COMPILER i686-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER i686-linux-gnu-g++)
set(CMAKE_C_FLAGS_INIT "-msse4.1 -mfpmath=sse")
set(CMAKE_CXX_FLAGS_INIT "-msse4.1 -mfpmath=sse")
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-i386;-L;/usr/i686-linux-gnu)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig")
