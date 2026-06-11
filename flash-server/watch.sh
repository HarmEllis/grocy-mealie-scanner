#!/usr/bin/env bash
# Starts the HTTP server and watches for new builds.
# When firmware/build/grocy-mealie-scanner.bin changes, bins/ is updated automatically.
# Run once inside the devcontainer; kill with Ctrl+C.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../firmware/build"
BIN="${BUILD_DIR}/grocy-mealie-scanner.bin"

copy_bins() {
    "${SCRIPT_DIR}/sync-bins.sh"
    echo "[watch] bins updated — $(date '+%H:%M:%S')"
}

copy_bins

echo "[watch] HTTP server on http://localhost:8080 — watching for new builds..."
python3 -m http.server 8080 --directory "$SCRIPT_DIR" &
SERVER_PID=$!
trap "kill $SERVER_PID 2>/dev/null" EXIT

LAST_MOD=$(stat -c '%Y' "$BIN")
while true; do
    sleep 2
    CURRENT_MOD=$(stat -c '%Y' "$BIN" 2>/dev/null || echo "$LAST_MOD")
    if [[ "$CURRENT_MOD" != "$LAST_MOD" ]]; then
        LAST_MOD="$CURRENT_MOD"
        copy_bins
    fi
done
