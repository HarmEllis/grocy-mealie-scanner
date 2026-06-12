# GM67 firmware improvement plan

Proposal derived from the GM67 Bar Code Reader Module User Manual V1.1
(Hangzhou Grow, Dec 2019), Appendix 6 "Serial Port Instruction". This is a
planning artifact for review/acceptance, not yet implemented.

## 1. Current state

`firmware/main/gm67.c` is a **passive text reader**:

- Opens UART1 at 9600 8N1, spawns `reader_task`.
- Treats incoming bytes as ASCII, splits on `CR`/`LF`, with an idle-flush
  fallback for units that send no terminator.
- Filters payloads with `code_is_plausible()` (len 4..64, `[A-Za-z0-9_-]`).
- Debounces identical codes within `debounce_ms` in firmware.

It **never talks to the module**. Correct operation depends on the unit having
been pre-configured by hand with setup barcodes (continuous/auto-sense mode,
TTL serial output, a terminator). `gm67.h` and `BOARD_NOTES.md` both state this
dependency explicitly. A unit shipped in a different default state silently
fails until someone scans the right config barcodes from the paper manual.

## 2. What the manual unlocks

Appendix 6 documents a **host serial command protocol** (Motorola/Zebra
SSI-style). The ESP32-S3 can drive the module over the existing TX line instead
of only listening on RX.

### Frame format

```
[LEN] [OPCODE] [04] [08] [00] [param bytes...] [CHK_HI] [CHK_LO]
```

- `LEN` = count of **all pre-checksum bytes including the `LEN` byte itself**
  (i.e. everything except the two checksum bytes). Check against the tables:
  ACK `04 D0 04 00` → 4 bytes → `LEN=04`; continuous `07 C6 04 08 00 8A 04` →
  7 bytes → `LEN=07`. The framed-reply parser must use this same definition.
- Checksum = 16-bit two's complement of the sum of every byte except the two
  checksum bytes. Verified against the manual's examples, e.g. continuous mode
  `07 C6 04 08 00 8A 04` → sum `0x167` → `0x10000-0x167 = 0xFE99` → `FE 99`. ✓
- **Every command we need is printed in the manual as a literal byte string.**
  We hardcode `static const uint8_t[]` arrays and do **not** synthesize frames
  at runtime. (A checksum helper is worth adding only to *validate* the tables.)

### Control commands (Table 6-1)

| Command | Bytes |
|---|---|
| CMD_ACK (module → host) | `04 D0 04 00 FF 28` |
| CMD_NAK RESEND | `05 D1 04 00 01 FF 25` |
| CMD_NAK BAD_CONTEXT | `05 D1 04 00 02 FF 24` |
| CMD_NAK DENIED | `05 D1 04 00 06 FF 20` |
| SCAN_ENABLE / SCAN_DISABLE | `04 E9 04 00 FF 0F` / `04 EA 04 00 FF 0E` |
| START_DECODE / STOP_DECODE (host mode) | `04 E4 04 00 FF 14` / `04 E5 04 00 FF 13` |
| LED_ON / LED_OFF | `05 E7 04 00 01 FF 0F` / `05 E8 04 00 01 FF 0E` |
| Custom buzzer (1 beep) | `05 E6 04 00 01 FF 10` |
| SLEEP / WAKEUP | `04 EB 04 00 FF 0D` / (none — any serial byte wakes it) |
| PARAM_DEFAULTS | `04 C8 04 00 FF 30` |
| RESET | `04 FA 04 00 FE FE` |

### Parameter commands (Table 6-2) we care about

| Setting | Bytes |
|---|---|
| Trigger: Continuous | `07 C6 04 08 00 8A 04 FE 99` |
| Trigger: Induction (auto-sense) | `07 C6 04 08 00 8A 09 FE 94` |
| Trigger: Host | `07 C6 04 08 00 8A 08 FE 95` |
| Scan data send format: Code only | `07 C6 04 08 00 EB 00 FE 3C` |
| Terminator: CR LF | `08 C6 04 08 00 F2 05 01 FE 2E` |
| Terminator: Forbid | `08 C6 04 08 00 F2 05 00 FE 2F` |
| STX/ETX: Forbid | `08 C6 04 08 00 F2 B7 00 FD 7D` |
| Decoded packet format: raw | `07 C6 04 08 00 EE 00 FE 39` |
| Decoded packet format: packet | `07 C6 04 08 00 EE 01 FE 38` |
| Good-read beep: On / Off | `07 C6 04 08 00 38 01 FE EE` / `…38 00 FE EF` |
| Comm interface: Serial | `08 C6 04 08 00 F2 01 00 FE 33` |
| 2D global enable: Forbid | `08 C6 04 08 00 F2 50 00 FD E4` |
| Same-code delay: Infinite (forbid dup) | `08 C6 04 08 00 F2 C9 09 FD 62` |
| Single scan time: 3s | `08 C6 04 08 00 F2 FA 03 FD 37` |
| Sensitivity: High | `08 C6 04 08 00 F2 04 01 FE 2F` |

## 3. Design decisions (with rationale)

### 3.1 Configure on **every boot**, not once via an NVS flag

The GM67 shares the ESP power rail, so it cold-boots on every power cycle.
If its PARAM writes land in **RAM** (not flash), a one-time "provision then set
NVS flag" scheme would configure the module once and then never again — and the
next power cycle reverts it to its stored config, silently breaking scanning.

Why every-boot is the right *default*: if writes are volatile, re-applying each
boot is *required*, and a one-time "provision then set NVS flag" scheme would
silently break on the next power cycle. Every-boot is safe in that case.

Persistence is a **prerequisite to verify, not an assumption to wave away.**
This plan does **not** assert that an unchanged persistent write is a module-side
no-op — that is unverified. Open question §6.1 gates the policy on a bench test:

- **Writes are volatile (RAM):** every-boot config, unconditionally. No NVS
  state, consistent with `AGENTS.md` ("keep NVS writes rare").
- **Writes are persistent to nonvolatile storage:** do **not** rewrite every
  boot (flash wear). The intended path is **idempotent provisioning** — read each
  parameter and `PARAM_SEND` only the ones that differ — but this is **deferred,
  not yet specified**: the manual lists `PARAM_REQUEST` without giving its
  request/response frame layout, so the read side must be captured on hardware
  first (§6.5). No spare-parameter "version stamp" — that writes undocumented
  module state and still wouldn't prove the real params match. Until the
  `PARAM_REQUEST` frames are known, the persistent case has **no shipping
  implementation**.

Phase 1 therefore builds only the volatile/every-boot path. Structure the config
as a single command table so that, once §6.1 resolves and the `PARAM_REQUEST`
frames (§6.5) are known, the idempotent path can reuse it (read, compare,
conditionally send) without re-listing commands.

### 3.2 Best-effort, fail-open — with a real frame parser

Configuration runs inside the `gm67` owning task. **Startup is not guaranteed
quiet:** a serially reachable module may boot in continuous/induction mode and
already be emitting raw scan bytes before/during configuration — disabling our
own callbacks does not silence the module. So there is **one parser for both
startup and runtime**: the signature-preserving stream demultiplexer in §3.4,
which tolerates a scan in flight while a command's ACK is awaited. We therefore
do **not** issue a startup `SCAN_DISABLE`/`SCAN_ENABLE` pair (that would risk
leaving the scanner disabled if a later command aborts — see step 4). A module
that never ACKs (wrong baud, USB-KBW mode, older firmware) still falls through to
the passive reader — we never brick the read path on a config failure.

"Send bytes and `uart_read_bytes`/`strstr` for `CMD_ACK`" is **not** a safe
receive design: UART reads can return partial frames, several frames at once,
interleaved scan data, or a *late* ACK from a previous timed-out command — and
because `CMD_ACK` carries no command identifier, a stale ACK could falsely mark
the next command as succeeded. The transaction layer must therefore:

1. **Establish quiescence per transaction — destructive flush only at startup.**
   At startup, before any scan is delivered to the app, `uart_flush_input` is
   safe (pre-config scans are discarded anyway). At **runtime this must not drop
   bytes**: a destructive flush before a beep/LED command would silently eat a
   legitimate scan that arrived just before it. Instead, at runtime, drain
   pending bytes *through* the demux (replies consumed, scan bytes preserved for
   the text parser) and only then mark the command pending.
2. **Match replies via the §3.4 signature demultiplexer, not a `LEN`-first read.**
   Recognize only the small fixed set of exact, checksum-valid reply signatures
   (`CMD_ACK`, the three `CMD_NAK` variants, plus any Phase-2 control reply);
   route every non-matching byte to the text parser. A naive `LEN`-first parse
   would misread a scan's leading ASCII byte (e.g. `0x38` = '8') as a 56-byte
   frame length and swallow the ACK — in *both* phases, since the module can
   stream scans at boot too.
3. **Validate exactly** — a transaction succeeds only on a checksum-valid
   `CMD_ACK` received *within its own* window. On `CMD_NAK RESEND`, resend
   (≤3 tries). On `CMD_NAK BAD_CONTEXT`/`DENIED`, log and move on.
4. **Abort on the first timeout.** At 9600 8N1 a byte is ~1.04 ms and the longest
   reply is a handful of bytes (≈10 ms on the wire), so the ACK window is set to
   a generous fixed bound (≈150 ms). But the manual documents **no maximum module
   response latency**, so we cannot prove a late, untagged ACK won't arrive during
   a *later* command's window and falsely satisfy it. With no verified latency
   bound, a post-timeout "quiet interval" is unprovable — therefore on the **first
   transaction timeout, abort the remaining configuration sequence** and fall
   through to the passive reader. (A quarantine-and-continue scheme may replace
   this only after max response latency is bench-measured — §6.) Concrete
   constants: ACK window `GM67_ACK_TIMEOUT_MS = 150`, resend cap `3`, abort after
   the first non-recovered timeout.
5. **Bound and discard** — cap the accumulator; on overflow or a malformed frame,
   reset the buffer so a garbage byte cannot wedge the parser.

### 3.3 Keep the existing text reader as the supported path

Phase 1 drives the module into exactly the output the current reader already
handles correctly (Code-only payload + deterministic CR LF terminator, raw not
packet). The working parser does **not** change. The packet-format parser is a
separate, optional, higher-risk phase (§4.3).

### 3.4 Single UART owner (required before Phase 2)

The UART has **one** owner and the design must say so explicitly. Today
`reader_task` blocks continuously in `uart_read_bytes`; once Phase 2 lets the
app send commands at runtime, an app-side command helper and the reader would
race for the same RX bytes — the reader would swallow command ACKs and the
helper would swallow barcode data.

Resolution: route **all** GM67 RX/TX through the single `gm67` task. It owns a
small **command queue** and demultiplexes incoming bytes into *reply frames*
(ACK/NAK, consumed by the pending command) vs *scan data* (handed to `s_cb`).

**Stream demultiplexer (one parser, startup and runtime).** A scan can be
mid-flight whenever a command is sent — at runtime *and* at boot, since the
module may power up scanning — so the demux never trusts a `LEN`-first read: a
barcode's leading ASCII byte would be misparsed as a frame length and swallow the
ACK. Instead, only ever expect a *small, fixed set* of reply frames (`CMD_ACK` and
the three `CMD_NAK` variants — plus any control reply a Phase-2 command needs)
and match them as **exact, checksum-valid byte signatures**:

- **Always strip control signatures, regardless of pending state.** The demux
  scans for one of the known signatures; a complete, checksum-valid match is
  *always consumed and removed* from the stream — whether or not a transaction is
  pending. This is what keeps a fire-and-forget command's unsolicited ACK from
  leaking into the text parser as garbage. Whether a *result is delivered* is a
  separate question: a consumed reply is handed to the pending acknowledged
  transaction if one is active, and otherwise simply dropped.
- **Every non-matching byte is preserved for the text parser.** A run of bytes
  that begins to look like a frame prefix but fails to complete or fails the
  checksum is **rolled back** in full to the text parser (partial-prefix
  rollback), so concatenated `scan+reply` or `reply+scan` input loses nothing.
- During startup the text-parser output is simply discarded until config
  completes (we do not surface pre-config scans).

The app never touches the UART directly — it calls a thin control API. Runtime
commands in scope are **cosmetic and fire-and-forget only:**

```
/* Cosmetic: fire-and-forget. Return value = queue admission only. */
esp_err_t gm67_beep(void);              /* custom buzzer, good-read cue */
esp_err_t gm67_led(bool on);            /* LED_ON / LED_OFF            */
```

- **`gm67_beep`/`gm67_led` — queue admission only.** `ESP_OK` means "enqueued",
  not "the GM67 ACKed"; a dropped beep/LED is cosmetic, so no ACK is awaited.
  Their ACK is still consumed by the always-on signature stripping above (no
  pending transaction, so the reply is dropped) and never reaches the text parser.
- **No acknowledged runtime command, including module-level scan enable/disable,
  until max response latency is bench-verified (§6.4).** §3.2 step 4 shows that,
  without a latency bound, retrying or issuing a *different* acknowledged command
  after an ambiguous timeout is unsafe — e.g. a late `disable` ACK could falsely
  confirm a subsequent `enable`, leaving scanning dead. A reconciliation loop does
  not fix this; it depends on the same unproven correlation. So a
  `gm67_set_scan_enabled()`-style acknowledged call is **deferred** to a later
  phase gated on §6.4. Protecting the keyboard screens stays in **software** —
  `handle_scan` already drops scans in `APP_PROPOSAL`/`APP_SEARCH`, which needs no
  module command and cannot brick scanning. Module-level disable is only a
  power/cleanliness optimization and is not worth the correlation risk now.

There is **no separate pre-task owner**: `gm67_init()` starts this task and
blocks on a readiness signal; the task runs the boot configuration in its startup
phase (through this same demux), signals ready, then enters the command/data
loop. The startup transaction helper and the runtime command queue are the *same*
mechanism, exercised at startup vs at runtime. This API and the owning-task
change are added to `gm67.h` and the file-change summary (§8).

## 4. Phased plan

### Phase 1 — Active boot configuration (recommended, low-risk, additive)

Goal: make the device deterministic for any **serially reachable** module,
removing the manual setup-barcode dependency, without touching the parser.

`gm67_init()` starts the single `gm67` owning task (§3.4) and waits for its
readiness signal. The task's **startup phase** runs configuration as a sequence
of framed transactions through the §3.2/§3.4 signature demux (drain → signature
match → checksum-validate → per-window ACK, **abort on first non-recovered
timeout**), discarding any pre-config scan bytes, then signals ready and enters
its normal command/data loop:

1. Send a wake byte; small delay (covers a unit asleep in induction mode).
2. Apply, in order, each as a framed transaction (best-effort, fail-open). No
   startup `SCAN_DISABLE`/`SCAN_ENABLE` — the demux tolerates live scan traffic,
   and bracketing the sequence with disable/enable would risk an aborted run
   leaving the scanner disabled (§3.2):
   - Comm interface → Serial.
   - Trigger mode → **Continuous** (`8A 04`). (Induction is a Phase 2 opt-in.)
   - Scan data send format → Code only (`EB 00`).
   - Terminator → CR LF (`F2 05 01`).
   - STX/ETX → Forbid.
   - Decoded packet format → raw (`EE 00`).
   - Good-read beep → On.
   - Single scan time → 3s.
   - (Open question §6.2) 2D global → Forbid, unless we want Mealie QR/URLs.
3. Signal ready; enter the command/data loop. The barcode text-parsing logic
   itself is the former `reader_task` path unchanged; what is new is that it now
   runs *behind* the §3.4 demux (which first strips any reply frames) rather than
   reading the UART directly.

**Command policy (per §3.1):** Phase 1 ships only the **volatile/every-boot**
path — apply the full table at each startup. The persistent/idempotent path is
deferred until the `PARAM_REQUEST` frames are captured (§6.5); the table is
structured so it can later be reused for read-compare-send.

The transaction helper is the one specified in §3.2 — **not** a
`uart_read_bytes`+`strstr` ACK match.

Touches: `gm67.c` (new config block + helper), `gm67.h` (doc comment update),
`BOARD_NOTES.md` / `gm67.h` (drop the "must be pre-configured" caveat, replace
with the reachability caveat from §5).

### Phase 2 — Feedback, power & tuning (low-risk, additive)

- **Visual result feedback via the board WS2812 (GPIO42), mirroring the existing
  UI colour palette** — green `0x35c98c` (found), amber `0xf5c13d` (not-found),
  coral `0xe8674a` (error). Pure ESP-side (`led_strip`/RMT): no runtime UART
  command, no ACK/latency dependency, no demux interaction. Hook points already
  exist in `main.c` (`show_product`, `ui_show_not_found`, `show_error`).
  Rationale for WS2812 over the GM67's own buzzer/LED: (1) **timing** — found/
  not-found/error is known only *after* the network round-trip, whereas the
  GM67's decode beep fires immediately at read time; a module-driven result beep
  would arrive seconds later as a confusing second beep. (2) **decoupling** — any
  runtime UART command reopens the command/ACK-correlation path Phase 1
  deliberately closed. The decode beep (already on from Phase 1) stays the audio
  channel; no app-driven sound.
- **User-toggleable beep and RGB, on-device.** Both default on but independently
  switchable from a small **on-device settings screen** (reachable from idle),
  backed by two NVS flags in `storage.c`. The config captive portal is unsuitable
  (only reachable on first-setup or after the destructive 5 s BOOT factory reset).
  The LED toggle is an ESP-side bool gating the WS2812 flash (instant). The beep
  toggle reconfigures the GM67 good-read beep via **one best-effort runtime
  PARAM_SEND** at the moment of toggling — accepted as the lone runtime UART
  command because it is rare, user-initiated, carries no brick risk (worst case:
  a missed/spurious beep), and is not hard-dependent on its ACK. The settings-
  screen LVGL layout + idle entry affordance follow a maintainer-provided design
  (per the design-driven UI in `design/`).
- **Induction (auto-sense) trigger mode** for lower idle power and less white-LED
  wear — a hands-free grocery scanner idles most of the time. Adds tunables:
  sensitivity (`F2 04 01` high), stable induction time. Ambient-light dependent,
  so it ships behind Phase 1's Continuous default until bench-verified.
- **Keyboard-screen protection stays in software.** Keeping a stray scan from
  interrupting typing on `APP_PROPOSAL`/`APP_SEARCH` is already handled by
  `handle_scan` dropping scans there. Module-level `SCAN_DISABLE` would be cleaner
  and save a little power, but it is an **acknowledged operational command** and
  is **deferred until max response latency is bench-verified (§3.4, §6.4)** — a
  dropped/mis-correlated re-enable could silently brick scanning, a worse failure
  than the software guard it would replace. Revisit once §6.4 is resolved.

### Phase 3 — Packet decode format (OPTIONAL, needs hardware, higher-risk)

The manual provides the **command to switch** to packet output (`EE 01`) but
lists `DECODE_DATA: None` and **does not document the inbound packet byte
layout** (header/length/CodeID/checksum). Packet parsing would therefore rest on
an *inferred* Zebra-SSI structure and must be reverse-engineered on real
hardware (capture the bytes, confirm framing, confirm the host must ACK each
`DECODE_DATA`). Benefits if pursued: exact length (no idle-flush heuristic),
symbology via Code ID, integrity via checksum. **Recommendation:** do not build
this blind; defer until a unit is on the bench. Keep raw text mode otherwise.

### Independent small parser fixes (fold into Phase 1)

- Once the terminator is forced to CR LF, the idle-flush path becomes a rare
  fallback rather than a routine code path — keep it but note it as fallback.
- Re-confirm `code_is_plausible` covers the target symbologies: EAN-8 (8),
  EAN-13 (13), UPC-A (12), UPC-E (8) are all digit-only and pass. If Mealie
  QR/URLs are in scope (§6), the `[A-Za-z0-9_-]`-only filter must be widened.

## 5. Limitations to state honestly

- **Not unconditionally "plug-and-play".** Boot-config only reaches the module
  if it is already listening on TTL serial at 9600 8N1. A unit shipped in
  USB-KBW or at a different baud cannot be reached by the ESP, and the
  setup-barcode bootstrap is still required for that first contact. The accurate
  claim is "deterministic configuration for serially-reachable modules."

## 6. Open questions

1. **Param persistence (RAM vs flash).** Determines whether every-boot config is
   *required* or *merely redundant*, and whether flash-wear is a concern.
   Resolve by bench test (configure, power-cycle without re-config, observe).
2. **Mealie QR / product 2D codes in scope?** If yes, keep 2D enabled and widen
   the plausibility filter; if no, disable 2D to cut misreads.
3. **Do PARAM_SEND commands ACK** the same as control commands? Assumed yes
   (SSI). Best-effort handling makes a wrong assumption non-fatal.
4. **Maximum module response latency** — undocumented, and it gates more than
   startup: until bench-measured, (a) startup aborts on the first timeout
   (§3.2.4) rather than continue with uncorrelated ACKs, and (b) **no acknowledged
   runtime command** is issued — module-level scan enable/disable is deferred
   (§3.4) and keyboard protection stays in software. Measuring it would unlock a
   quarantine-and-continue scheme, a tighter ACK window, and a safe acknowledged
   `gm67_set_scan_enabled()`.
5. **`PARAM_REQUEST` request/response frames** — listed in the manual but the
   layout is not given; required before the persistent/idempotent config path
   (§3.1) can be built. Capture on hardware.
6. **Inbound packet layout** for Phase 3 — undocumented; needs capture.

## 7. Verification

- **Here:** build-only, **inside the workspace devcontainer** as mandated by
  `AGENTS.md` (not a standalone `espressif/idf` image). Find the running
  container and build as the `node` user after sourcing the IDF profile:

  ```bash
  C=$(docker ps --filter "label=devcontainer.local_folder=$PWD" --format '{{.Names}}')
  docker exec -u node "$C" bash -c \
    "source /etc/profile.d/esp-idf.sh && \
     cd /workspaces/grocy-mealie-scanner/firmware && idf.py build"
  ```

  Then confirm the image still fits the 3 MB OTA slot
  (`stat -c%s build/grocy-mealie-scanner.bin`) and that the partition table
  still carries `ota_0`/`ota_1` (`idf.py partition-table`).
- **Host unit tests for the demultiplexer (required — it is the riskiest new
  code).** The signature-stripping + partial-prefix-rollback parser is stateful
  and its failure mode is *silent* barcode loss/corruption, which a bench test
  cannot reliably exhaust. Factor it as a **pure function over a byte buffer**
  (no UART/RTOS dependency) — feeding bytes in, emitting `(scan-text | reply |
  nothing)` — so it runs on the host. Cover, with input chunked at *arbitrary*
  boundaries (one byte at a time and in larger blocks): scan-only; reply-only;
  `scan+reply`; `reply+scan`; a partial signature that never completes; a
  signature prefix that fails the checksum (must roll the whole run back to the
  text parser); accumulator overflow; an ACK split across reads; and a reply
  arriving with/without a pending transaction. These are deterministic and need
  no hardware.
- **Bench (required before shipping Phase 1):** confirm ACKs arrive, scanning
  works end-to-end after a config reset, and resolve open questions §6.1/§6.2
  (and §6.4 before any acknowledged runtime command is added). The bench test
  validates electrical/protocol behavior, not stream-parser edge cases — those
  are the host tests above.

## 8. File-change summary

| File | Change |
|---|---|
| `firmware/main/gm67.c` | Single owning task with a command queue; **pure host-testable demux** (always-strip control signatures + partial-prefix rollback) separated from UART/RTOS glue; ACK/NAK transaction helper (startup-only flush → signature match → checksum-validate → per-window match, abort on first timeout); `gm67_configure()` command table run at boot |
| `firmware/main/gm67.h` | Update contract comment (no longer "must be pre-configured"); reachability caveat; add fire-and-forget control API `gm67_beep()`, `gm67_led()`. (Acknowledged `gm67_set_scan_enabled()` deferred to §6.4.) |
| `firmware/main/main.c` | (Phase 2) beep/LED hooks in state transitions. Keyboard-screen scan guard stays in `handle_scan` (no module command). |
| `firmware/BOARD_NOTES.md` | Replace pre-config note with reachability caveat + summary of pushed config |
| `firmware/test/` (host) | Unit tests for the demux pure function — chunked input across the §7 case matrix |
