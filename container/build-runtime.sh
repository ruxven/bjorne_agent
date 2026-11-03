#!/usr/bin/env bash
# Build the bjorne_agent runtime image in rootless podman.
#
# Usage: container/build-runtime.sh [tag]
#
# The image is built from the repository root and tagged "bjorne_agent" by default.
# The binary is the entrypoint, so the image is run with the task as the
# argument and the config delivered by a mount or by BJORNE_AGENT_CONFIG.
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tag=${1:-bjorne_agent}

if [ "$#" -gt 1 ]; then
  echo "usage: container/build-runtime.sh [tag]" >&2
  exit 2
fi

echo "==> building runtime image $tag"
podman build \
  -t "$tag" \
  -f "$repo_dir/container/Containerfile.runtime" \
  "$repo_dir"

echo "==> image contents"
podman image inspect "$tag" --format 'size: {{.Size}} bytes, entrypoint: {{json .Config.Entrypoint}}, user: {{.Config.User}}'

echo "==> smoke test"
podman run --rm "$tag" --help >/dev/null

echo "==> $tag is ready"
echo "  podman run --rm -v \$PWD/bjorne_agent.json:/etc/bjorne_agent/bjorne_agent.json:ro \\"
echo "    -v \$PWD:/workspace -e GAB_API_KEY $tag \"your task\""
