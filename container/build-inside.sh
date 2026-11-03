#!/bin/sh
# Configure, build, test, and install bjorne_agent inside a container.
#
# Usage: build-inside.sh <static|dynamic|notls>
#
# Runs as the image's builder user against the repository mounted at /src. Each
# mode gets its own build directory so the CMake caches never mix. The install
# tree lands beside the build directory, so the host gets the same layout the
# host build produces.
set -eu

mode=${1:-}
case "$mode" in
  static)  openssl=static ;;
  dynamic) openssl=shared ;;
  notls)   openssl=disabled ;;
  *)
    echo "usage: build-inside.sh <static|dynamic|notls>" >&2
    exit 2
    ;;
esac

build_dir="/src/build-container-$mode"
install_dir="/src/build-container-$mode/install"

cmake -S /src -B "$build_dir" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBJORNE_AGENT_OPENSSL="$openssl"

cmake --build "$build_dir" --parallel "$(nproc)"

ctest --test-dir "$build_dir" --output-on-failure

# The install tree holds the binary, the core library, and the prompt files.
cmake --install "$build_dir" --prefix "$install_dir"
