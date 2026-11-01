# CMake toolchain for LG webOS TV: 32-bit ARMv7-A SOFT-float userspace.
# (eim e_flags 0x5000200 "soft-float ABI"; hard-float binaries are rejected by
# the TV loader.) softfp = soft calling convention + VFP codegen (TV has VFPv3).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-linux-gnueabi-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabi-g++)

set(CMAKE_C_FLAGS_INIT "-mfloat-abi=softfp -march=armv7-a -mfpu=vfpv3")
set(CMAKE_CXX_FLAGS_INIT "-mfloat-abi=softfp -march=armv7-a -mfpu=vfpv3")

# Find root for find_library() (ezinject's cmake/ezinject.cmake needs C_LIBRARY).
# NOTE: no CMAKE_SYSROOT — the cross compiler knows its own sysroot, and setting
# it makes ld double-prefix absolute paths from libc.so's linker script.
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabi)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Never try to RUN a test binary during configure (we can only compile here).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
