#!/usr/bin/env bash
# Host unit tests for the GM67 protocol/demux layer and the OTA semver logic.
# Pure C, no ESP-IDF needed.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
bin="$(mktemp)"
trap 'rm -f "$bin"' EXIT
cc -Wall -Wextra -I "$here/../main" -o "$bin" \
    "$here/test_gm67_demux.c" "$here/../main/gm67_proto.c"
"$bin"
cc -Wall -Wextra -I "$here/../main" -o "$bin" \
    "$here/test_semver.c" "$here/../main/semver.c"
"$bin"
