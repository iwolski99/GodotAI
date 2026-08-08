#!/usr/bin/env bash
# Verify the editor GDExtension binary exists and matches the current git commit.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

COMMIT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
BIN_DIR="project/addons/godot_ai_os/bin"

case "$(uname -s)" in
	Linux*)  LIB="$BIN_DIR/libgodot_ai_os.linux.editor.$(uname -m).so" ;;
	Darwin*) LIB="$BIN_DIR/libgodot_ai_os.macos.editor.framework/libgodot_ai_os.macos.editor" ;;
	MINGW*|MSYS*|CYGWIN*) LIB="$BIN_DIR/libgodot_ai_os.windows.editor.x86_64.dll" ;;
	*) echo "Unsupported platform"; exit 1 ;;
esac

echo "Expected git commit: $COMMIT"
echo "Library path:        $LIB"

if [[ ! -f "$LIB" ]]; then
	echo "ERROR: GDExtension library is missing."
	echo "Build from the repo root on branch $(git branch --show-current 2>/dev/null || echo '?'):"
	echo "  scons platform=<linux|windows|macos> target=editor -j<cores>"
	exit 1
fi

echo "Library timestamp:   $(date -r "$LIB" 2>/dev/null || stat -c '%y' "$LIB" 2>/dev/null || echo unknown)"
echo
echo "After rebuilding:"
echo "  1. Fully QUIT Godot (reloadable=false — restarting the project is not enough)"
echo "  2. Re-open the project that contains project/addons/godot_ai_os/"
echo "  3. In the AI Agent dock log, look for: Extension build <commit> (<date>) loaded"
echo "  4. In Settings, scroll down — Max tool turns + Extension build: <commit> (<date>)"
