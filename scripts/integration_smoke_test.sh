#!/usr/bin/env bash
# Headless integration smoke test for the agent bridge.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="$ROOT/project"
LOG="/tmp/godot_ai_os_smoke.log"
PID=""

cleanup() {
  if [[ -n "$PID" ]] && kill -0 "$PID" 2>/dev/null; then
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "Starting headless editor..."
godot --headless --editor --path "$PROJECT" >"$LOG" 2>&1 &
PID=$!

echo "Waiting for session file..."
for _ in $(seq 1 60); do
  if [[ -f "$PROJECT/.godot/ai_agent_os/session.json" ]]; then
    break
  fi
  sleep 1
done

if [[ ! -f "$PROJECT/.godot/ai_agent_os/session.json" ]]; then
  echo "Session file never appeared. Log:"
  tail -n 80 "$LOG" || true
  exit 1
fi

echo "Pinging bridge..."
python3 "$ROOT/clients/python/godot_ai_client.py" "$PROJECT" ping

echo "Reading world model..."
python3 "$ROOT/clients/python/godot_ai_client.py" "$PROJECT" get_world_model '{"max_depth": 1}'

echo "Listing tools..."
python3 "$ROOT/clients/python/godot_ai_client.py" "$PROJECT" list_tools '{}' | head -c 500

echo ""
echo "Smoke test passed."
