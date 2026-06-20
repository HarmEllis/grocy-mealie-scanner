# GM67 Barcode Scanner Protocol Reference

This document provides a comprehensive technical reference for the Hangzhou Grow GM67 barcode scanner module's serial protocol (Appendix 6 of the user manual). It is intended for developers implementing host-mode control and configuration over UART.

## 1. Serial Port Configuration

By default, the GM67 uses the following UART settings:

- **Baud Rate:** 9600 bps (default, configurable up to 115200)
- **Data Bits:** 8
- **Stop Bits:** 1
- **Parity:** None
- **Flow Control:** None
- **Logic Level:** TTL (3.3V / 5V compatible)

### 1.1 Wake-up Sequence
If the scanner is in a low-power sleep state, it may ignore the first byte received. To ensure the module is awake before sending a command:
1. Send a single `0x00` byte.
2. Wait for **50ms**.
3. Send the instruction frame.

---

## 2. Frame Format (SSI Style)

The protocol uses binary frames with a leading length byte and a trailing 16-bit checksum.

### 2.1 Host to Module (Instruction)
```text
[LEN] [OPCODE] [LENS] [ADDR_H] [ADDR_L] [DATA...] [CHK_HI] [CHK_LO]
```

- **LEN:** Total number of bytes in the frame *excluding* the two checksum bytes.
- **OPCODE:** The type of operation (e.g., `0xC6` for Write Parameter).
- **LENS:** Optional length of the following payload (Address + Data). Often `0x04` for 1-byte parameter writes.
- **ADDR:** 2-byte internal address.
    - `0x0800`: Configuration/Parameter memory.
    - `0x0400`: System action memory.
- **DATA:** The value(s) being written.
- **CHK:** 16-bit checksum.

### 2.2 Checksum Calculation
The checksum is the **16-bit two's complement of the sum** of all bytes in the frame (from `LEN` up to the last `DATA` byte).

**Algorithm (Pseudo-code):**
```c
uint32_t sum = 0;
for (int i = 0; i < len; i++) {
    sum += frame[i];
}
uint16_t checksum = (uint16_t)(0x10000u - (sum & 0xFFFFu));
```

**Example:** Trigger Continuous Mode `07 C6 04 08 00 8A 04`
- Sum: `0x07 + 0xC6 + 0x04 + 0x08 + 0x00 + 0x8A + 0x04 = 0x167`
- Two's Complement: `0x10000 - 0x167 = 0xFE99`
- Resulting Frame: `07 C6 04 08 00 8A 04 FE 99`

---

## 3. Control Commands (Opcode Table)

These commands perform immediate actions or indicate status.

| Command | Opcode | Full Hex Frame | Description |
|---|---|---|---|
| **CMD_ACK** | `0xD0` | `04 D0 04 00 FF 28` | Success acknowledgment (Module → Host) |
| **CMD_NAK** | `0xD1` | `05 D1 04 00 [ERR] [CHK]` | Failure notification (Module → Host) |
| **SCAN_ENABLE** | `0xE9` | `04 E9 04 00 FF 0F` | Enable scanning engine |
| **SCAN_DISABLE** | `0xEA` | `04 EA 04 00 FF 0E` | Disable scanning engine |
| **START_DECODE** | `0xE4` | `04 E4 04 00 FF 14` | Trigger scan (Host Mode) |
| **STOP_DECODE** | `0xE5` | `04 E5 04 00 FF 13` | Stop scan (Host Mode) |
| **LED_ON** | `0xE7` | `05 E7 04 00 01 FF 0F` | Turn on illumination LED |
| **LED_OFF** | `0xE8` | `05 E8 04 00 01 FF 0E` | Turn off illumination LED |
| **BEEP_CUE** | `0xE6` | `05 E6 04 00 01 FF 10` | Sound the buzzer (1 beep) |
| **SLEEP** | `0xEB` | `04 EB 04 00 FF 0D` | Enter sleep mode |
| **PARAM_DEFAULTS**| `0xC8` | `04 C8 04 00 FF 30` | Restore factory parameters |
| **RESET** | `0xFA` | `04 FA 04 00 FE FE` | Software reboot |

### 3.1 NAK Error Codes
- `0x01`: RESEND (Checksum error)
- `0x02`: BAD_CONTEXT (Command not allowed in current state)
- `0x06`: DENIED (Invalid address or data)

---

## 4. Parameter Settings (PARAM_SEND - Opcode 0xC6)

Parameter changes use address `0x0800`. Note that some parameters use extended data formats.

### 4.1 Trigger & Mode
| Setting | Hex Frame |
|---|---|
| **Manual Trigger** | `07 C6 04 08 00 8A 00 FE 9D` |
| **Continuous Scan** | `07 C6 04 08 00 8A 04 FE 99` |
| **Induction (Auto-sense)** | `07 C6 04 08 00 8A 09 FE 94` |
| **Host Trigger Mode** | `07 C6 04 08 00 8A 08 FE 95` |

### 4.2 Data Format & Interface
| Setting | Hex Frame |
|---|---|
| **Comm Interface: Serial** | `08 C6 04 08 00 F2 01 00 FE 33` |
| **Baud Rate: 9600** | `07 C6 04 08 00 9C 06 FE 85` |
| **Baud Rate: 115200** | `07 C6 04 08 00 9C 0A FE 81` |
| **Raw Mode (Text)** | `07 C6 04 08 00 EE 00 FE 39` |
| **Packet Mode** | `07 C6 04 08 00 EE 01 FE 38` |
| **Send Code Only (No ID)** | `07 C6 04 08 00 EB 00 FE 3C` |

### 4.3 Terminator & Suffixes
| Setting | Hex Frame |
|---|---|
| **Terminator: CR LF** | `08 C6 04 08 00 F2 05 01 FE 2E` |
| **Terminator: None** | `08 C6 04 08 00 F2 05 00 FE 2F` |
| **STX/ETX: Off** | `08 C6 04 08 00 F2 B7 00 FD 7D` |

### 4.4 Hardware Feedback
| Setting | Hex Frame |
|---|---|
| **Good-read Beep: On** | `07 C6 04 08 00 38 01 FE EE` |
| **Good-read Beep: Off** | `07 C6 04 08 00 38 00 FE EF` |
| **Volume: High** | `07 C6 04 08 00 8C 00 FE 9B` |
| **Volume: Low** | `07 C6 04 08 00 8C 02 FE 99` |

### 4.5 Performance Tuning
| Setting | Hex Frame |
|---|---|
| **Single Scan Time: 3s** | `08 C6 04 08 00 F2 FA 03 FD 37` |
| **Sensitivity: High** | `08 C6 04 08 00 F2 04 01 FE 2F` |
| **Same-code Delay: Infinite**| `08 C6 04 08 00 F2 C9 09 FD 62` |

---

## 5. Querying Parameters (PARAM_REQUEST - Opcode 0xC7)

The `PARAM_REQUEST` command is used to read the current value of a parameter.

### 5.1 Frame Layout
```text
[05] [C7] [04] [00] [PARAM_ID] [CHK_HI] [CHK_LO]
```

- **PARAM_ID:** The ID of the parameter (e.g., `0x8A` for Trigger Mode).
- **Special ID `0xFE`:** Request ALL parameters.

### 5.2 Examples
- **Request All Parameters:** `05 C7 04 00 FE FE 32`
- **Request Trigger Mode:** `05 C7 04 00 8A FF A6`

### 5.3 Response (PARAM_SEND)
The module responds with a `0xD6` opcode frame containing the parameter ID and its current value.

---

## 6. Decode Data Formats

The module can send scan results in two formats: **Raw** and **Packet**.

### 6.1 Raw Mode (Default)
In Raw mode, the module sends the barcode characters as a simple ASCII stream, followed by the configured terminator (e.g., `CR LF`).

### 6.2 Packet Mode (DECODE_DATA)
When Packet Mode is enabled (`EE 01`), scan results are wrapped in a structured packet:

| Offset | Field | Length | Description |
|---|---|---|---|
| 0 | **Header** | 2 bytes | Always `0x02 0x00` |
| 2 | **Type** | 1 byte | Always `0x00` (Decoded Data) |
| 3 | **Length** | 2 bytes | Length of the Data field (Big-Endian) |
| 5 | **Data** | N bytes | The raw barcode text (ASCII) |
| 5+N | **LRC** | 1 byte | Longitudinal Redundancy Check |

**LRC Calculation:**
The LRC is a single byte XOR of all bytes from the **Type** field through the end of the **Data** field.

---

## 7. Protocol Flow Example

Typical configuration sequence at boot:

1. **Wake up:** Send `0x00`, wait 50ms.
2. **Apply Settings:** Send multiple `PARAM_SEND` frames.
    - Host sends `07 C6 04 08 00 8A 04 FE 99` (Continuous Mode).
    - Module replies `04 D0 04 00 FF 28` (ACK).
3. **Handle Errors:**
    - If Host receives `05 D1 04 00 01 FF 25` (NAK RESEND), it retransmits the last frame.
4. **Operation:**
    - Module streams scan data when a barcode is detected.
