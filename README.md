# Buddy

English | [简体中文](README.zh-CN.md)

> AI agents: read [AGENTS.md](AGENTS.md) first.

This repository is the open, customizable edition of Buddy, a USB desk device (ESP32-S3) with an **RGB LED** and a
**solenoid** that makes the topper (a mushroom or any 3D-printed figure) jump. It replaces the official firmware with
an open firmware (prebuilt in `firmware/bin`, sources in `firmware/`), controlled over USB by the `buddy` CLI and
Python library in `buddy/`. Custom features are host scripts in `apps/`, or firmware changes when a script cannot do
it.

## Usage

Requires Python 3.9+.

```bash
pip install -e .              # in the repository folder
buddy setup                   # first time: installs the open firmware and checks the device
buddy led breathe green
buddy jump 2
buddy notify meeting          # blue blink + three jumps
buddy remind 25m "Take a break"
buddy led off
```

```python
from buddy import Buddy

with Buddy() as b:
    b.notify("meeting")
    b.wait(60)
    b.led_off()
```

## Documentation

- [AGENTS.md](AGENTS.md): commands, Python API and rules
- [docs/recipes.md](docs/recipes.md): app recipes
- [docs/protocol.md](docs/protocol.md): serial protocol
- [docs/firmware.md](docs/firmware.md): firmware sources and build (ESP-IDF v6.0)
- [docs/official-firmware.md](docs/official-firmware.md): switching between official and open firmware

## License

Firmware and software: Apache-2.0, except the official firmware image in `firmware/official/`.
