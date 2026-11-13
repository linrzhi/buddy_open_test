# Firmware

The open firmware sources live in `firmware/` (ESP-IDF project, target `esp32s3`, 4 MB flash).

## Environment

- ESP-IDF v6.0: install it with the
  [Espressif guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) and work in an
  ESP-IDF terminal.
- Python packages for flashing come with ESP-IDF (or `pip install -e .` for the `buddy` tool).

## Build and flash

```bash
cd firmware
idf.py build                 # output in firmware/build
idf.py -p <PORT> flash       # PORT: the 303A:1001 serial port
```

Or from the repository root: `buddy build` and `buddy flash firmware/build --full`.

- The device is flashed over USB-Serial-JTAG (`303A:1001`). After `idf.py flash` the chip may stay in download mode;
  replug USB once.
- If the device does not show `303A:1001` (official firmware, or a firmware without USB-Serial-JTAG), enter download mode
  first: see [official-firmware.md](official-firmware.md).
- Logs (`ESP_LOG`) go to UART0 (GPIO43 TX / GPIO44 RX, 115200), not to USB.
- `buddy build --export` copies the build into `firmware/bin` (the prebuilt images used by `buddy setup`).

Custom firmware does not have to keep the text protocol in `buddy_cmd` or work with the `buddy` tool.

## Layout

```text
firmware/
  CMakeLists.txt          project, version (PROJECT_VER)
  sdkconfig.defaults      configuration defaults
  partitions.csv
  main/app_main.c         entry: board, drivers, USB transport, command handler
  components/
    board/                pin map (board_config.h), hardware revision detection and name
    drivers/              drv_led (RGB effects), drv_solenoid (jump sequences, coil protection)
    transport/            usb_serial_port: USB-Serial-JTAG I/O
    buddy_cmd/            text protocol (docs/protocol.md)
  bin/                    prebuilt images, flasher_args.json, manifest.json
  official/               official firmware image (for restoring, not open source)
```

## Hardware

| Function | GPIO |
|---|---|
| RGB LED R / G / B | 6 / 4 / 5 |
| Boost enable | 1 |
| Solenoid | 38 |
| Boot button (pinhole, download mode at power-up) | 0 |
| Hardware revision ADC | 8 |
| USB D- / D+ | 19 / 20 |
| UART0 TX / RX (logs) | 43 / 44 |

- Keep solenoid on-time short and the average duty low (see `drv_solenoid`); the coil overheats otherwise.
- Keeping USB on USB-Serial-JTAG keeps one-cable flashing; USB OTG (TinyUSB) takes over GPIO19/20 and removes that port.
