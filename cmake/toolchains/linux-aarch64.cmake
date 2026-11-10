# CMake toolchain file for a 64 bit ARM (aarch64) Linux cross build. This is
# the architecture of the Raspberry Pi 3, 4 and 5 running a 64 bit OS, and of
# most other ARM64 Linux machines.
#
#   sudo apt-get install g++-aarch64-linux-gnu
#
# The compiler prefix is fixed to the aarch64 target, because the job that uses
# this file builds for aarch64 only.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(ARM_TRIPLET aarch64-linux-gnu)

set(CMAKE_C_COMPILER ${ARM_TRIPLET}-gcc)
set(CMAKE_CXX_COMPILER ${ARM_TRIPLET}-g++)

# Look for headers and libraries in the cross sysroot, but keep the host
# programs, such as the compiler itself, on the host path.
set(CMAKE_FIND_ROOT_PATH /usr/${ARM_TRIPLET})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The executables land in bin/ under the build directory, which is the same
# layout the install rules produce on a native build.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
