# Cross toolchain for Linux arm64 (Raspberry Pi 4/5 64-bit OS, other aarch64 boards) from an x86-64 Ubuntu host with arm64 multiarch packages.
# Build-time tools the build produces (state table, ucode fixup, file_to_c) run through qemu-user; shaders use the host's x64 DXC.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Scheduling tuned for the Raspberry Pi 4's Cortex-A72; it adds no instructions, so the build still runs on any ARMv8 CPU.
set(CMAKE_C_FLAGS_INIT "-mtune=cortex-a72")
set(CMAKE_CXX_FLAGS_INIT "-mtune=cortex-a72")
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64;-L;/usr/aarch64-linux-gnu)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
