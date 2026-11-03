#!/usr/bin/env bash
# Build and test bjorne_agent in rootless podman for one or more link modes.
#
# Usage: container/build.sh [static] [dynamic] [notls]
#
# With no arguments all three modes run. Each mode builds an image whose
# builder user has the caller's uid and gid, then runs the build with
# --userns=keep-id so the bind-mounted source stays writable and the artifacts
# come out owned by the caller. Nothing compiles as root.
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
uid=$(id -u)
gid=$(id -g)

if [ "$uid" -eq 0 ]; then
  echo "refusing to run as root: the builder must be an unprivileged user" >&2
  exit 1
fi

if [ "$#" -eq 0 ]; then
  modes=(static dynamic notls)
else
  modes=("$@")
fi

for mode in "${modes[@]}"; do
  containerfile="$repo_dir/container/Containerfile.$mode"
  if [ ! -f "$containerfile" ]; then
    echo "no such build mode: $mode (expected $containerfile)" >&2
    exit 2
  fi

  tag="bjorne_agent-build-$mode"
  echo "==> $mode: building image $tag"
  podman build \
    --build-arg UID="$uid" \
    --build-arg GID="$gid" \
    -t "$tag" \
    -f "$containerfile" \
    "$repo_dir/container"

  echo "==> $mode: compiling and testing"
  podman run --rm \
    --userns=keep-id \
    -v "$repo_dir:/src:Z" \
    -w /src \
    "$tag" \
    sh /src/container/build-inside.sh "$mode"
done

echo "==> artifacts"
for mode in "${modes[@]}"; do
  binary="$repo_dir/build-container-$mode/bjorne_agent"
  [ -x "$binary" ] || continue
  echo "--- $mode"
  file -b "$binary"
  ldd "$binary" 2>&1 | sed 's/^/    /' || true
done
