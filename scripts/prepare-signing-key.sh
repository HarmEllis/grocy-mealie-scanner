#!/usr/bin/env bash
# Prepare the ESP-IDF app signing key at firmware/secure_boot_signing_key.pem.
#
# Release builds should provide ESP_SECURE_BOOT_SIGNING_KEY_B64, containing
# the base64-encoded RSA-3072 PEM private key. Local builds may keep the
# ignored PEM file at the target path. CI validation can pass
# --generate-if-missing to use a throwaway key; never use that mode for release
# artifacts.

set -euo pipefail

MODE="${1:-}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
KEY_PATH="${REPO_ROOT}/firmware/secure_boot_signing_key.pem"

if [[ -n "${ESP_SECURE_BOOT_SIGNING_KEY_B64:-}" ]]; then
    printf '%s' "$ESP_SECURE_BOOT_SIGNING_KEY_B64" | base64 -d > "$KEY_PATH"
elif [[ ! -f "$KEY_PATH" && "$MODE" == "--generate-if-missing" ]]; then
    espsecure.py generate_signing_key --version 2 "$KEY_PATH"
elif [[ ! -f "$KEY_PATH" ]]; then
    cat >&2 <<'EOF'
ERROR: firmware/secure_boot_signing_key.pem is missing.

For release builds, set the GitHub Actions secret
ESP_SECURE_BOOT_SIGNING_KEY_B64 to the base64-encoded PEM private key.

For local builds, put the ignored PEM file at:
  firmware/secure_boot_signing_key.pem

To create a new key inside the ESP-IDF environment:
  espsecure.py generate_signing_key --version 2 firmware/secure_boot_signing_key.pem
EOF
    exit 1
fi

chmod 600 "$KEY_PATH"

openssl rsa -in "$KEY_PATH" -check -noout >/dev/null
key_bits="$(openssl rsa -in "$KEY_PATH" -text -noout \
    | sed -n 's/^Private-Key: (\([0-9][0-9]*\) bit.*/\1/p')"
if [[ "$key_bits" != "3072" ]]; then
    echo "ERROR: expected RSA-3072 private key, got ${key_bits:-unknown} bits" >&2
    exit 1
fi

digest_file="$(mktemp)"
trap 'rm -f "$digest_file"' EXIT
espsecure.py digest_sbv2_public_key --keyfile "$KEY_PATH" --output "$digest_file" >/dev/null
