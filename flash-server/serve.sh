#!/usr/bin/env bash
# Run inside the devcontainer after `idf.py build`.
# Copies the built binaries to flash-server/bins/ and starts a local HTTP server on port 8080.
# VS Code forwards port 8080 → open http://localhost:8080 in Chrome on your local machine.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "==> Copying binaries..."
"${SCRIPT_DIR}/sync-bins.sh"

echo "==> Serving flash-server/ on http://localhost:8080"
echo "    Open in Chrome on your local machine and click Install."
echo ""
cd "$SCRIPT_DIR"
python3 -m http.server 8080
