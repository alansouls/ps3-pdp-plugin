# PDP Riffmaster HID Report Map

Oct 3, 2026 · @Alan

## Overview

The Riffmaster sends a 64-byte DualShock 4-style input report with ID `0x01`. All inputs from the test sequence live in just six bytes: 5, 6, 7, 44, 45 and 46. Byte 0 is the first byte of the report.

- **Device:** Performance Designed Products - PDP RiffMaster Guitar for PS4 (USB HID, macOS path `DevSrvsID:4294990788`)
- **Capture:** `dump.txt`, 196 reports over 54 s, captured with `--changes-only`
- **Idle report** (nothing pressed, guitar at rest):

```
01 80 80 80 80 0f 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00 00 00 80 00 00 00 80 00 00 00 00 00 26 00 00 ...
```

Byte 45 at idle drifts between `0x15` and `0x33`. That drift is the tilt sensor, which explains the reports you saw from small touches.

## Byte map

| Byte | Idle value | Meaning | DS4 equivalent |
| --- | --- | --- | --- |
| 0 | `01` | Report ID | Report ID |
| 1–4 | `80 80 80 80` | Centered stick axes; did not move in this capture | LX, LY, RX, RY |
| 5, low nibble | `f` | Hat switch: strum bar (`f` = neutral) | D-pad |
| 5, high nibble | `0` | Green, red, yellow and blue frets | Face buttons |
| 6 | `00` | Orange fret, Select, Start | L1, Share, Options |
| 7 | `00` | PS button | PS |
| 25 | `01` | Constant | — |
| 35, 39 | `80` | Constant | — |
| 44 | `00` | Whammy, 0x00–0xFF | (vendor) |
| 45 | `15`–`33` | Tilt, roughly 0x1D–0xF2 | (vendor) |
| 46 | `00` | Fret bitmask, one bit per fret | (vendor) |

All other bytes stayed `00` for the whole capture.

## Buttons

Every fret is reported twice: once as a DS4 button in bytes 5–6, and once as a bit in byte 46. The strum bar is the hat switch, and Start, Select and PS use the standard DS4 bits.

| Action | Byte | Mask / value | Pressed report (bytes 5 6 7 … 46) |
| --- | --- | --- | --- |
| Green | 5 / 46 | `0x20` / `0x01` | `2f 00 00` … `01` |
| Red | 5 / 46 | `0x40` / `0x02` | `4f 00 00` … `02` |
| Yellow | 5 / 46 | `0x80` / `0x04` | `8f 00 00` … `04` |
| Blue | 5 / 46 | `0x10` / `0x08` | `1f 00 00` … `08` |
| Orange | 6 / 46 | `0x01` / `0x10` | `0f 01 00` … `10` |
| Strum up | 5 (low nibble) | `0x0` (hat N) | `00 00 00` … `00` |
| Strum down | 5 (low nibble) | `0x4` (hat S) | `04 00 00` … `00` |
| Start | 6 | `0x20` | `0f 20 00` … `00` |
| Select | 6 | `0x10` | `0f 10 00` … `00` |
| PS button | 7 | `0x01` | `0f 00 01` … `00` |

To decode:

- **Frets:** use `byte46 & mask`. One byte covers all five frets, so it's the simplest to read.
- **Strum:** use `byte5 & 0x0F`: `0` = up, `4` = down, `0xF` = neutral.
- **Face buttons:** use `byte5 & 0xF0` and leave the hat bits out of the mask.

## Analog axes

Whammy is a clean 8-bit axis in byte 44. Tilt is a coarse, noisy 8-bit axis in byte 45.

| Axis | Byte | Rest | Full | Update rate | Notes |
| --- | --- | --- | --- | --- | --- |
| Whammy | 44 | `0x00` | `0xFF` | about every 8 ms | Smooth and monotonic. All 3 sweeps hit exactly 0x00 and 0xFF. No noise at rest. |
| Tilt | 45 | `0x15`–`0x33` | `0xEE`–`0xF2` | about every 200 ms (5 Hz) | Jitters by about ±8 at rest. Peaks varied across the 3 sweeps: 0xEE, 0xF2, 0xEE. |

Tilt noise: byte 45 changed at 20.6 s and 22.6 s with nothing pressed, and again at 53.8 s after the sweeps. This is the source of the reports you saw from small touches.

Treat tilt as a threshold, not a raw value. For example, count it as active above about `0x80`, and only release it below about `0x50` so it doesn't flicker. That matches how Rock Band star-power tilt behaves.

## Capture timeline

Each press shows up as a press report followed by an idle report 0.15–0.3 s later. The table lists them in the order you performed them.

| Time (s) | Reports | Action | Change |
| --- | --- | --- | --- |
| 24.80 | #4–5 | Green | b5 `2f`, b46 `01` |
| 26.64 | #6–7 | Red | b5 `4f`, b46 `02` |
| 27.74 | #8–9 | Yellow | b5 `8f`, b46 `04` |
| 28.97 | #10–11 | Blue | b5 `1f`, b46 `08` |
| 30.19 | #12–13 | Orange | b6 `01`, b46 `10` |
| 31.43 | #14–15 | Strum up | b5 `00` |
| 32.60 | #16–17 | Strum down | b5 `04` |
| 34.36 | #18–19 | Start | b6 `20` |
| 35.62 | #20–21 | Select | b6 `10` |
| 37.02 | #22–23 | PS button | b7 `01` |
| 40.13–44.54 | #24–164 | Whammy ×3 | b44 00→FF→00 |
| 45.98–52.38 | #165–193 | Tilt ×3 | b45 \~26→EE/F2→\~26 |

## Open questions

The sequence didn't cover these. A second capture would settle them:

- [ ] **Chords:** press two or more frets at once to confirm the bits combine (for example, green + red gives b46 `0x03`).
- [ ] **Solo frets:** the Riffmaster's lower solo frets may use other bits in byte 46 or byte 6.
- [ ] **Joystick and D-pad left/right:** these probably map to bytes 1–4 and hat values `2`/`6`.
- [ ] **Byte 25 = `0x01`:** this byte never changed. It might be battery or connection status.
