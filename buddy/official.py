# SPDX-License-Identifier: Apache-2.0
"""Switch a device running the official firmware into download mode, without the pinhole.

The official firmware appears as a USB HID device (VID:PID 303A:83D7). Recent versions accept a
download-mode request as a HID Feature Report (see docs/official-firmware.md). The chip then
restarts into its ROM download mode, which shows up as the USB-Serial-JTAG port 303A:1001.

Needs the optional ``hidapi`` package (``import hid``); without it everything here reports
"not available" and the pinhole remains the way in.
"""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Any, List, Optional

from .device import find_ports

OFFICIAL_VID = 0x303A
OFFICIAL_PID = 0x83D7
USAGE_PAGE = 0xFF00
USAGE = 0x0001

REPORT_ID = 0xD0
MAGIC = b"BDY!"
PROTOCOL_VERSION = 0x01
CAP_DOWNLOAD = 0x01
CMD_ENTER_DOWNLOAD = 0x01
REPORT_LEN = 1 + 7  # report ID + payload


@dataclass
class OfficialDevice:
    path: bytes
    release: int  # bcdDevice


def _hid() -> Optional[Any]:
    try:
        import hid  # type: ignore[import-not-found]
    except ImportError:
        return None
    return hid if hasattr(hid, "device") and hasattr(hid, "enumerate") else None


def available() -> bool:
    return _hid() is not None


def find_official() -> List[OfficialDevice]:
    """Devices running the official firmware (one entry per device)."""
    hid = _hid()
    if hid is None:
        return []
    items = hid.enumerate(OFFICIAL_VID, OFFICIAL_PID)
    # Windows lists one entry per top-level collection; other systems may report usage 0.
    tagged = [i for i in items if i.get("usage_page") == USAGE_PAGE and i.get("usage") == USAGE]
    return [
        OfficialDevice(i["path"], i.get("release_number") or 0)
        for i in (tagged or items)
    ]


def _open(dev: OfficialDevice) -> Any:
    hid = _hid()
    handle = hid.device()
    handle.open_path(dev.path)
    return handle


def supports_download(dev: OfficialDevice) -> bool:
    """True if this firmware version accepts the download-mode request."""
    try:
        handle = _open(dev)
    except Exception:  # noqa: BLE001 - busy or gone
        return False
    try:
        data = bytes(handle.get_feature_report(REPORT_ID, REPORT_LEN))
    except Exception:  # noqa: BLE001 - older firmware has no such report
        return False
    finally:
        handle.close()
    # Some hidapi backends drop the report ID from the returned buffer.
    if data[:1] == bytes([REPORT_ID]):
        data = data[1:]
    return len(data) >= 6 and data[:4] == MAGIC and data[4] == PROTOCOL_VERSION and bool(data[5] & CAP_DOWNLOAD)


def enter_download(dev: OfficialDevice, wait_s: float = 8.0) -> Optional[str]:
    """Ask the device to restart into download mode; returns the new 303A:1001 port, or None."""
    before = {p.device for p in find_ports()}
    handle = _open(dev)
    try:
        handle.send_feature_report(bytes([REPORT_ID, *MAGIC, PROTOCOL_VERSION, CMD_ENTER_DOWNLOAD, 0]))
    finally:
        try:
            handle.close()
        except Exception:  # noqa: BLE001 - the device may already be gone
            pass
    deadline = time.monotonic() + wait_s
    while time.monotonic() < deadline:
        time.sleep(0.2)
        new = [p.device for p in find_ports() if p.device not in before]
        if new:
            time.sleep(0.5)  # let the OS finish setting up the port
            return new[0]
    return None


class SwitchError(Exception):
    pass


PINHOLE = "unplug USB, hold the pinhole button with a SIM pin, plug in, release, then retry"


def switch_to_download() -> str:
    """Find a device with the official firmware and put it into download mode; returns the port."""
    devices = find_official()
    if not devices:
        hint = "" if available() else " (hidapi not installed: run `pip install -e .`)"
        raise SwitchError(f"no Buddy found{hint}. Check the cable, or {PINHOLE}.")
    if len(devices) > 1:
        raise SwitchError("several Buddy devices found; plug in only one")
    if not supports_download(devices[0]):
        raise SwitchError(f"this firmware cannot switch to download mode; {PINHOLE}.")
    print("official firmware found; switching to download mode", flush=True)
    port = enter_download(devices[0])
    if port is None:
        raise SwitchError(f"download port did not appear; {PINHOLE}.")
    return port
