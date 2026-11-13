# AGENTS.md

This repository is the open, customizable edition of Buddy, a USB desk device (ESP32-S3) with an **RGB LED** and a
**solenoid** that makes the topper (a mushroom or any 3D-printed figure) jump. It replaces the official firmware with
an open firmware (prebuilt in `firmware/bin`, sources in `firmware/`), controlled over USB (USB-Serial-JTAG,
`303A:1001`) by the `buddy` CLI and Python library in `buddy/`. Custom features are host scripts in `apps/`, or
firmware changes when a script cannot do it. Follow the workflow below.

## Workflow

1. **Install** (once):
   ```bash
   git clone <repo-url> buddy && cd buddy   # skip if already in the repository
   pip install -e .
   buddy setup        # flashes the prebuilt firmware (firmware/bin) if needed, then checks the device
   ```
   Later sessions: `buddy doctor`. No ESP-IDF needed for this.
2. **Decide** where the feature runs:
   - Host script (default): anything built from LED colors/effects and jumps, triggered by time, network, files,
     calendars, CI, other programs or AI hooks.
   - Firmware change: only if it needs something the commands below cannot do (new hardware behavior, new effect
     timing on the device, running without a computer). Then follow **Firmware**.
3. **Build** the app as `apps/<name>.py` with the Python library (see `examples/`, `docs/recipes.md`).
4. **Verify**: run it, confirm the user sees/feels the effect, and fix until it works.
5. **Hand over**: tell the user how to start and stop it; set up autostart only if asked.

## CLI

All commands accept `--json` and `-p PORT`.

```bash
buddy setup | doctor | info
buddy led red | led 255 120 0 | led blink blue 300 | led breathe green 2000 | led rainbow | led off
buddy jump [count 1-255] [duration_ms 1-5000] [interval_ms 0-5000] | jump stop
buddy notify <preset> [--hold S]      # presets: done meeting alert water pomodoro info party
buddy remind 15:30|25m <text> [--preset P] [--hold S]
buddy hook done|off|<preset>          # for AI tool hooks; never fails
buddy send "<raw command>"            # see docs/protocol.md
buddy flash [firmware_dir] [--full]   # default: prebuilt firmware in firmware/bin
buddy build                           # = idf.py build in firmware/
```

Exit codes: 0 ok, 1 device error, 2 usage, 3 not found, 4 flash/build failed.

## Python

```python
from buddy import Buddy

with Buddy() as b:              # auto-detects the port
    b.notify("meeting")         # LED + jumps
    b.led("blue", "blink", 300)
    b.jump(2)
    b.wait(60)                  # use instead of time.sleep()
    b.led_off()
```

API: `led(color, mode="constant|blink|breathe|rainbow", ms=None)`, `led_off()`, `jump(count, duration_ms, interval_ms)`,
`jump_stop()`, `notify(preset)`, `wait(seconds)`, `info()`, `command(line)`, `reboot()`.
Errors: `BuddyNotFoundError`, `BuddyCommandError` (`.code`: see `docs/protocol.md`).

## App rules

- The device has no user input; effects end on a timer (`--hold`, `b.wait()`).
- In long-running scripts wait with `b.wait()`, so other `buddy` commands and `buddy flash` can borrow the port.
- Solenoid thermal budget: 12 s burst, 20% average duty. On `busy`, wait or use the LED only.
- Autostart: the OS scheduler (Task Scheduler / launchd / systemd --user).
- `apps/` is ignored by git, so user apps survive `git pull`.

## Firmware

- Requires ESP-IDF v6.0 (target esp32s3). Loop: edit → `buddy build` → `buddy flash firmware/build` → `buddy doctor`.
- Sources in `firmware/`: `components/{board,drivers,transport,buddy_cmd}`, entry `main/app_main.c`. Details: `docs/firmware.md`.
- New command: add it to `execute_line()` and `s_usage` in `firmware/components/buddy_cmd/src/buddy_cmd.c`, then update
  `docs/protocol.md`, `buddy/device.py` and `tests/`. Replies are one line: `OK`, `OK {json}` or `ERR <code> <message>`.
- Keep USB on USB-Serial-JTAG. Do not add TinyUSB / USB OTG.

## Never

- Run `espefuse`, enable Secure Boot or flash encryption (irreversible).
- Run `erase-flash` or raw `esptool` writes; use `buddy setup` / `buddy flash`.
- Change `firmware/partitions.csv` or the bootloader unless the user asks.
- Bypass solenoid limits or the thermal budget.
- Print logs on the USB port.

## If the device is not found

- `buddy setup` handles a new device by itself.
- Otherwise: unplug, hold the pinhole button with a SIM pin, plug in, release, then `buddy flash --full`.
- "Device descriptor request failed": cable or USB port problem.
- Switching between official and open firmware: `docs/official-firmware.md`.
