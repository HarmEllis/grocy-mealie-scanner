#!/usr/bin/env bash
# Single source of truth for which binaries land in flash-server/bins/.
# Reads manifest.json, locates each listed part under firmware/build/
# (esp-idf nests bootloader/ and partition_table/), and copies it.
# Used by serve.sh, watch.sh, and CI so the manifest cannot drift from
# what is actually published.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/../firmware/build}"
BINS_DIR="${BINS_DIR:-${SCRIPT_DIR}/bins}"
MANIFEST="${SCRIPT_DIR}/manifest.json"

if ! command -v jq >/dev/null 2>&1; then
    echo "ERROR: jq is required but not installed." >&2
    exit 1
fi

mkdir -p "$BINS_DIR"

while IFS= read -r part_path; do
    filename="$(basename "$part_path")"
    src="$(find "$BUILD_DIR" -name "$filename" -type f -print -quit)"
    if [[ -z "$src" ]]; then
        echo "ERROR: $filename listed in manifest.json but not found under $BUILD_DIR" >&2
        echo "       Run: cd firmware && idf.py build" >&2
        exit 1
    fi
    cp "$src" "$BINS_DIR/$filename"
done < <(jq -r '.builds[].parts[].path' "$MANIFEST")
