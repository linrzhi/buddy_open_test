# Buddy serial protocol

The firmware exposes a virtual serial port on the chip's USB-Serial-JTAG (VID:PID `303A:1001`).
Most users should use the `buddy` CLI or Python library; this page is for writing your own client.

## Connecting

- Any baud rate (USB-Serial-JTAG ignores it).
- **Set DTR and RTS low (False) before opening the port**, otherwise the chip resets or enters download mode.
  With pyserial: create `serial.Serial()`, set `port`, `dtr = False`, `rts = False`, then call `open()`.
- Only one program can open the port at a time.
- No logs are printed on this port. The chip ROM may print a few lines on reset; ignore unknown lines.
- Handshake: send an empty line, then `info`, and check that `version` starts with `buddy-open`.

## Framing

- Host → device: one command per line, terminated by `\n` or `\r`, max 255 bytes, case-insensitive.
  Lines starting with `#` are ignored.
- Device → host: **exactly one reply line** per command, terminated by `\r\n`:
  - `OK` or `OK {json}` on success;
  - `ERR <code> <message>` on failure.
- Events: `EVT {json}`, which may appear between any two lines.
- Any other line: ignore.

## Commands

| Command | Description | Reply |
|---|---|---|
| `ping` | Connectivity check | `{"pong":true}` |
| `info` | Firmware version and hardware revision | `{"version":"buddy-open 1.0.0","hw":"REV1"}` |
| `help` | Command usage list | `{"commands":["ping","info",…]}` |
| `led` | Query LED state | `{"mode":"constant","r":0,"g":0,"b":255,"ms":0}` |
| `led off` | Turn off | new state, as above |
| `led <color>` / `led <r> <g> <b>` | Solid color | new state |
| `led blink <color>\|<r> <g> <b> [ms]` | Blink, `ms` = toggle interval 1-60000 (default 500) | new state |
| `led breathe <color>\|<r> <g> <b> [ms]` | Breathe, `ms` = period 100-60000 (default 2000) | new state |
| `led rainbow` | Color cycle | new state (`mode` = `rainbow`) |
| `jump [count] [duration_ms] [interval_ms]` | Solenoid jumps. count 1-255 (default 1), on-time 1-5000 ms (default 80), gap 0-5000 ms (default 150). A new command interrupts a running sequence | `{"count":1,"duration_ms":80,"interval_ms":150}` |
| `jump stop` | Stop jumping immediately | `OK` |
| `reboot` | Restart ~100 ms after replying | `OK` |

Color names: `red green blue white yellow orange cyan purple pink`.

### Solenoid thermal budget

Token bucket: 12000 ms of on-time capacity, refilled at 200 ms per second (20% long-term duty).

- `count × duration_ms` above 12000 in one command → `ERR out_of_range`.
- Not enough budget → `ERR busy solenoid cooling down, retry in N s`.

## Events

| Event | When |
|---|---|
| `EVT {"event":"boot"}` | Firmware started (seen only if the host is already connected) |

## Error codes

| code | Meaning |
|---|---|
| `unknown_command` | Unknown command |
| `bad_args` | Wrong number or format of arguments |
| `out_of_range` | Value out of range |
| `busy` | Solenoid cooling down |
| `hw_error` | Driver rejected the request |
| `line_too_long` | Line longer than 255 bytes |

## Example session

```text
> info
< OK {"version":"buddy-open 1.0.0","hw":"REV1"}
> led blink blue 300
< OK {"mode":"blink","r":0,"g":0,"b":255,"ms":300}
> jump 2
< OK {"count":2,"duration_ms":80,"interval_ms":150}
> led magenta
< ERR bad_args color must be a name (red green blue white yellow orange cyan purple pink) or three numbers 0-255
```

## Versioning

`info.version` follows semantic versioning (`buddy-open MAJOR.MINOR.PATCH`). New commands or JSON fields bump
MINOR; changing the meaning or format of an existing command bumps MAJOR.
