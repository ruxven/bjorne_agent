# CMake toolchain file for a 32 bit ARM (armhf) Linux cross build. This is the
# architecture of a Raspberry Pi running a 32 bit OS: the Pi 2 and later, and
# the Zero 2 W. The Pi 1, Zero and Zero W use ARMv6, which needs extra
# -march=armv6 flags on top of this file.
#
#   sudo apt-get install g++-arm-linux-gnueabihf
#
# The compiler prefix is fixed to the armhf target, because the job that uses
# this file builds for armhf only.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)

set(ARM_TRIPLET arm-linux-gnueabihf)

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
