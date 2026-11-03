# CMake toolchain file for a 64 bit MinGW-w64 cross build from Linux.
#
# The compiler is the GNU toolchain from the mingw-w64 packages, so the
# produced binary runs on Windows without a C runtime from Microsoft.
#
#   sudo apt-get install g++-mingw-w64-x86-64
#
# The compiler prefix is fixed to the 64 bit target, because the job that
# uses this file builds for x86-64 only.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TRIPLET x86_64-w64-mingw32)

set(CMAKE_C_COMPILER ${MINGW_TRIPLET}-gcc)
set(CMAKE_CXX_COMPILER ${MINGW_TRIPLET}-g++)
set(CMAKE_RC_COMPILER ${MINGW_TRIPLET}-windres)
set(CMAKE_AR ${MINGW_TRIPLET}-ar)
set(CMAKE_RANLIB ${MINGW_TRIPLET}-ranlib)
set(CMAKE_STRIP ${MINGW_TRIPLET}-strip)

# Look for headers and libraries in the cross sysroot, but keep the host
# programs, such as the compiler itself, on the host path.
set(CMAKE_FIND_ROOT_PATH /usr/${MINGW_TRIPLET})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The executables land in bin/ under the build directory, which is the same
# layout the install rules produce on a native build.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The MinGW runtime DLLs are copied next to the executable by the build.
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
