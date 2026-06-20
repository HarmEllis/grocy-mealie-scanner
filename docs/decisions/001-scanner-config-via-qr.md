# ADR 001: Configure GM67 scanner via QR codes, not serial PARAM_SEND batch

**Status:** Accepted  
**Date:** 2026-06-18

## Context

The GM67 UART barcode scanner supports two configuration paths:

1. **QR code setup** — scan special QR codes printed in the manual; the scanner stores the setting in its own NVS and the change is immediate.
2. **Serial PARAM_SEND** — the host sends binary frames (opcode 0xC6) over UART; the scanner ACKs and writes to NVS.

We implemented a batch PARAM_SEND sequence at boot (`gm67_configure()`) to push 8 settings (interface, baud, scan mode, terminator, etc.) without requiring manual QR scanning. This required:

- An NVS version key on the ESP side to skip re-applying on every boot.
- A 1500 ms startup delay so the scanner finishes its own boot sequence.
- A 500 ms per-command ACK timeout (scanner writes NVS before ACKing).
- Retry logic for NAK_RESEND frames.

## Problem

After the last command in the batch sequence the scanner consistently became unresponsive:

- The scan LED stayed on permanently.
- SCAN_DISABLE had no effect.
- Scanning stopped entirely.

The root cause was not fully isolated. Candidates include:

- The scanner entering an undefined state when all 8 parameters are written in quick succession.
- A checksum or framing error in one of the later commands causing the scanner to reject subsequent control commands.
- A firmware bug in our specific scanner unit's firmware version.

None of the individual fire-and-forget commands (SCAN_ENABLE, SCAN_DISABLE, beep toggle) were affected — only the full batch sequence broke the scanner.

## Decision

Remove the PARAM_SEND batch configuration sequence entirely. Configure the scanner once via QR codes (documented in `docs/scanner-setup.md`) and rely on the scanner's own NVS persistence.

Runtime settings that users may want to change (beep level, scan light, collimation) are applied as individual fire-and-forget PARAM_SEND commands from the settings screen. These work reliably — the scanner ACKs and persists them — unlike the boot-time batch.

## Consequences

- **Positive:** Scanner always boots into a known state; no risk of the batch sequence leaving it unresponsive.
- **Positive:** Simpler firmware — no NVS version tracking, no ACK-wait loop, no retry logic.
- **Positive:** Faster boot (no 1500 ms startup delay, no 8 × 500 ms ACK windows).
- **Negative:** First-time setup requires scanning four QR codes from the manual. This is a one-time operation.
- **Negative:** Replacing the scanner requires re-scanning the QR codes on the new unit.
- **Neutral:** Runtime beep/light/collimation changes still use PARAM_SEND (individual commands, not a batch), which is confirmed working.
