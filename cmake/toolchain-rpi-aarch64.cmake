# Cross-compilation toolchain for Raspberry Pi 4 running Raspberry Pi OS (Bookworm, aarch64).
# Requires an aarch64-linux-gnu cross toolchain and a sysroot with the Pi's dev packages.
# Set NEXUS_RPI_SYSROOT to the mounted/synced Pi sysroot before configuring.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

if(DEFINED ENV{NEXUS_RPI_SYSROOT})
  set(CMAKE_SYSROOT $ENV{NEXUS_RPI_SYSROOT})
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
