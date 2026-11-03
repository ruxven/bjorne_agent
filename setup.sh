#!/usr/bin/env bash
# setup.sh - interactive config generator for bjorne_agent
# Creates a bjorne_agent.json config and a matching .env from prompts
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== bjorne_agent setup ==="
echo

# Where to write the config. The binary searches the project local path first,
# then the user level path, so the two choices below match that order.
echo "Config location:"
echo "  1) project local  ./.bjorne_agent/bjorne_agent.json"
echo "  2) user level     $HOME/.config/bjorne_agent/bjorne_agent.json"
read -rp "Where should the config go? (default 1): " location
case "$location" in
  2) CONFIG_PATH="$HOME/.config/bjorne_agent/bjorne_agent.json" ;;
  *) CONFIG_PATH="$PWD/.bjorne_agent/bjorne_agent.json" ;;
esac
CONFIG_DIR="$(dirname "$CONFIG_PATH")"
ENV_PATH="$(dirname "$CONFIG_PATH")/.env"

# Endpoint
read -rp "Endpoint (e.g. https://api.openai.com/v1): " endpoint
[ -n "$endpoint" ] || { echo "Endpoint required."; exit 1; }

# Model
read -rp "Model id (e.g. gpt-4o): " model
[ -n "$model" ] || model="gpt-4o"

# API key env name
read -rp "Environment variable for API key (default OPENAI_API_KEY): " api_env
[ -n "$api_env" ] || api_env="OPENAI_API_KEY"

# Workspace
read -rp "Workspace path (default /workspace): " workspace
[ -n "$workspace" ] || workspace="/workspace"

# Compression prompt. The path is resolved relative to the config file, so the
# script points at a prompt file that actually exists.
PROMPT_CANDIDATES=(
  "$HOME/.local/opt/bjorne_agent/share/bjorne_agent/prompts/compression.md"
  "$REPO_DIR/config/prompts/compression.md"
)
PROMPT_PATH=""
for candidate in "${PROMPT_CANDIDATES[@]}"; do
  if [ -f "$candidate" ]; then
    PROMPT_PATH="$candidate"
    break
  fi
done
if [ -z "$PROMPT_PATH" ]; then
  read -rp "Compression prompt file? (default prompts/compression.md): " ptr
  [ -z "$ptr" ] && ptr="prompts/compression.md"
  PROMPT_PATH="$ptr"
fi

# Ignore self-signed? (default false = verify certs)
read -rp "Ignore self-signed certs? (y/N, default N): " ignore
if [[ "$ignore" == "y" || "$ignore" == "Y" ]]; then
  ignore_json="true"
  ignore_note="Self-signed certs accepted (ignore_self_signed=true)."
else
  ignore_json="false"
  ignore_note="Peer certificates verified (ignore_self_signed=false, default)."
fi

# Hostname for SNI (optional)
read -rp "SNI hostname override? (leave blank for endpoint host): " hostname

mkdir -p "$CONFIG_DIR"

JSON_FIELDS="  \"ignore_self_signed\": $ignore_json"
if [ -n "$hostname" ]; then
  JSON_FIELDS="$JSON_FIELDS, \"hostname\": \"$hostname\""
fi

cat > "$CONFIG_PATH" <<EOF
{
  "version": 1,
  "agent": {
    "system_prompt": "You are a coding agent. Help the user with their task.",
    "workspace": "$workspace"
  },
  "models": [
    {
      "provider": "openai-compatible",
      "endpoint": "$endpoint",
      "model": "$model",
      "context_window": 128000,
      "max_tokens": 4096,
      "temperature": 0.7,
      "api_key_env": "$api_env",
      "priority": 1,
      "compression_prompt_path": "$PROMPT_PATH",
$JSON_FIELDS
    }
  ]
}
EOF

echo "Created $CONFIG_PATH"
echo

echo "=== .env ==="
echo "Add your API key to $ENV_PATH (not tracked):"
echo "  export $api_env=sk-..."
echo

# Check if .env exists; append / create
if [ -f "$ENV_PATH" ]; then
  echo ".env already exists at $ENV_PATH"
else
  echo "$api_env=" > "$ENV_PATH"
  echo "Created $ENV_PATH (fill in $api_env)"
fi

echo
echo "=== Notes ==="
echo "$ignore_note"
echo "Compression instruction file: $PROMPT_PATH"
echo "Endpoint path is a prefix: /v1 + /chat/completions = /v1/chat/completions"
echo "Test with:  export \$(cat "$ENV_PATH" | xargs)"
echo "  bjorne_agent \"hello\""
