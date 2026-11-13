# Official firmware ⇄ open firmware

| Firmware | USB device |
|---|---|
| Official | HID `303A:83D7` |
| Open | serial `303A:1001` (USB-Serial-JTAG) |
| ROM download mode | serial `303A:1001` |

## Official → open

```bash
buddy setup
```

It sends the HID download-mode request below, waits for the ROM download port and flashes `firmware/bin` with `--full`.
Without the request (or if it fails), use the boot button below, then run `buddy flash --full`.

## Open → official

The official firmware image is in `firmware/official/`:

```bash
buddy flash firmware/official/buddy_official.bin --full
```

The open firmware provides `303A:1001`, so no button is needed. Afterwards the device shows up as HID `303A:83D7`.

## Download mode

### HID request (official firmware)

HID Feature Report, report ID `0xD0`, on the vendor collection (Usage Page `0xFF00`, Usage `0x0001`), 7-byte payload:

| Request | Payload | Meaning |
|---|---|---|
| GET_REPORT | `42 44 59 21` `01` `caps` `00` | `"BDY!"`, version 1, `caps` bit0 = supported |
| SET_REPORT | `42 44 59 21` `01` `01` `00` | enter download mode |

After SET_REPORT the device re-enumerates as `303A:1001` within about a second. Flash with esptool `--before no-reset`.
Implementation: `buddy/official.py`.

### Boot button (any firmware)

1. Unplug USB.
2. Hold the boot button through the pinhole with a SIM pin.
3. Plug in USB, then release.

The chip starts in ROM download mode (`303A:1001`) and stays there until flashed or replugged. This always works,
whatever firmware is on the device.
