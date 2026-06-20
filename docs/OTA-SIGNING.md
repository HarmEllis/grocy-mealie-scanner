# OTA Signing

The firmware uses ESP-IDF signed app images without hardware Secure Boot. This
does not burn eFuses and does not stop physical reflashing, but it does make OTA
updates reject images that are not signed by the same key as the currently
running signed app.

## Generate the release key

Run this inside the devcontainer:

```bash
cd /workspaces/grocy-mealie-scanner
source /etc/profile.d/esp-idf.sh
espsecure.py generate_signing_key --version 2 firmware/secure_boot_signing_key.pem
chmod 600 firmware/secure_boot_signing_key.pem
```

Back up `firmware/secure_boot_signing_key.pem` in a password manager or other
secret store. Do not commit it. Losing this key means you cannot produce OTA
updates that already-signed devices will accept.

## Add the GitHub secret

From the host, with `gh` authenticated:

```bash
# Linux/GNU coreutils:
gh secret set ESP_SECURE_BOOT_SIGNING_KEY_B64 \
  --repo HarmEllis/grocy-mealie-scanner \
  --body "$(base64 -w0 firmware/secure_boot_signing_key.pem)"

# macOS/BSD base64:
gh secret set ESP_SECURE_BOOT_SIGNING_KEY_B64 \
  --repo HarmEllis/grocy-mealie-scanner \
  --body "$(base64 < firmware/secure_boot_signing_key.pem | tr -d '\n')"
```

The release workflow decodes this secret into the ignored
`firmware/secure_boot_signing_key.pem` path before building. Normal CI uses a
throwaway key so pull requests can still verify that the project builds, but
only tagged releases use the real key. If the secret is missing, tagged release
builds fail before compiling firmware; that is intentional, because publishing
unsigned release firmware would strand already-signed devices.

## Local release-equivalent build

For local builds that should be signed with the release key, keep the ignored
key file at `firmware/secure_boot_signing_key.pem` and build normally inside the
devcontainer:

```bash
cd /workspaces/grocy-mealie-scanner
source /etc/profile.d/esp-idf.sh
./scripts/prepare-signing-key.sh
cd firmware
idf.py build
```

The app binary produced at `firmware/build/grocy-mealie-scanner.bin` contains
the signature block. The first signed firmware can be installed over Web Serial
or OTA from an unsigned build. After a signed build is running, future OTA
updates must be signed by the same key.

## Security boundaries

- This protects OTA update authenticity against network attackers.
- This does not enable hardware Secure Boot and does not burn eFuses.
- Anyone with physical access and flashing capability can still replace the
  bootloader or app over serial.
- NVS encryption remains disabled, so WiFi credentials and the device token are
  still readable by someone with physical flash access.
- Rotating the key requires shipping a transitional image before depending on
  the new key.
