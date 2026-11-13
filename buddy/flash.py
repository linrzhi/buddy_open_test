# SPDX-License-Identifier: Apache-2.0
"""Flashing and building (wraps esptool / idf.py).

Safety:
- By default only the app is written; bootloader, partition table and the data areas are kept.
  This is refused when the device still runs firmware with a different flash layout.
- ``full=True`` writes everything listed in flasher_args.json and clears the data area left by
  the previous firmware, or writes a merged image at 0x0. Use it for the first flash, recovery and
  going back to the official firmware.
- Never calls erase-flash or espefuse.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

from .device import find_ports

CHIP = "esp32s3"
FLASH_BAUD = "460800"
# Matches partitions.csv; flasher_args.json takes precedence when available.
DEFAULT_APP_OFFSET = 0x10000
PARTITION_TABLE_OFFSET = 0x8000
IMAGE_MAGIC = bytes([0xE9])
PARTITION_TABLE_MAGIC = bytes([0xAA, 0x50])
# Data area between the partition table and the app, cleared by a full flash.
DATA_AREA_OFFSET = 0x9000
DATA_AREA_SIZE = DEFAULT_APP_OFFSET - DATA_AREA_OFFSET
# First firmware version using the current flash layout; app-only flashing needs at least this.
LAYOUT_MIN_VERSION = (1, 0, 0)

# Repository layout: firmware sources in firmware/, local builds in firmware/build/,
# released (prebuilt) images in firmware/bin/.
FIRMWARE_DIR = Path("firmware")
BUILD_DIR = FIRMWARE_DIR / "build"
PREBUILT_DIR = FIRMWARE_DIR / "bin"
REPO_MARKER = FIRMWARE_DIR / "components" / "buddy_cmd"

PINHOLE_HINT = "unplug USB, hold the pinhole button with a SIM pin, plug in, release"


class FlashError(Exception):
    pass


@dataclass
class FlashPlan:
    images: List[Tuple[int, Path]] = field(default_factory=list)
    description: str = ""

    def esptool_args(self) -> List[str]:
        args: List[str] = []
        for offset, path in self.images:
            args += [hex(offset), str(path)]
        return args


def find_repo_root(start: Optional[Path] = None) -> Optional[Path]:
    """Repository root: walk up from the current directory, else use this package's checkout."""
    here = (start or Path.cwd()).resolve()
    package_root = Path(__file__).resolve().parents[1]
    for candidate in [here, *here.parents, package_root]:
        if (candidate / REPO_MARKER).is_dir():
            return candidate
    return None


def _blank_data_area() -> Path:
    """0xFF image of the data area, kept in one reusable file under the temp directory."""
    path = Path(tempfile.gettempdir()) / "buddy-cli" / f"blank_{DATA_AREA_SIZE:x}.bin"
    data = b"\xff" * DATA_AREA_SIZE
    if not path.is_file() or path.read_bytes() != data:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return path


def plan_from_path(path: Path, full: bool = False) -> FlashPlan:
    """Build a flash plan from an app .bin or a build directory."""
    path = path.resolve()
    if path.is_file():
        if full:
            # A merged image (bootloader + partition table + apps), e.g. the official firmware.
            with path.open("rb") as f:
                head = f.read(PARTITION_TABLE_OFFSET + 2)
            if head[:1] != IMAGE_MAGIC or head[PARTITION_TABLE_OFFSET:] != PARTITION_TABLE_MAGIC:
                raise FlashError(f"{path.name} is not a merged image; --full needs a firmware directory or merged .bin")
            return FlashPlan([(0x0, path)], f"full: merged image {path.name}")
        return FlashPlan([(DEFAULT_APP_OFFSET, path)], f"app {path.name}")

    args_file = path / "flasher_args.json"
    if not args_file.is_file():
        raise FlashError(f"no flasher_args.json in {path}; build first (buddy build)")
    meta = json.loads(args_file.read_text(encoding="utf-8"))

    def entry(key: str) -> Tuple[int, Path]:
        item = meta.get(key)
        if not item:
            raise FlashError(f"flasher_args.json has no {key}")
        return int(item["offset"], 0), path / item["file"]

    if full:
        images = sorted((int(off, 0), path / f) for off, f in meta["flash_files"].items())
        overlap = [p.name for o, p in images if DATA_AREA_OFFSET <= o < DEFAULT_APP_OFFSET]
        if overlap:
            raise FlashError(f"unexpected image layout ({', '.join(overlap)}); rebuild from this project")
        desc = "full: " + ", ".join(p.name for _, p in images) + ", clear old data"
        images.append((DATA_AREA_OFFSET, _blank_data_area()))
        images.sort()
    else:
        app_off, app_file = entry("app")
        images = [(app_off, app_file)]
        desc = f"app {app_file.name}"
    for _, file in images:
        if not file.is_file():
            raise FlashError(f"{file} not found; rebuild")
    return FlashPlan(images, desc)


def parse_version(text: str) -> Optional[Tuple[int, ...]]:
    """'buddy-open 1.2.3' or '1.2.3' -> (1, 2, 3); None if unparsable."""
    try:
        return tuple(int(x) for x in text.split()[-1].split(".")[:3])
    except (ValueError, IndexError):
        return None


def check_app_only(port: str) -> None:
    """Refuse app-only flashing unless the device runs firmware with the current layout."""
    from .device import Buddy, BuddyError

    try:
        # Runs while `buddy flash` holds the release request, so it must not take part in sharing.
        with Buddy(port, timeout=1.5, share_port=False) as b:
            version = str(b.device_info.get("version", ""))
    except BuddyError:
        raise FlashError(
            f"no Buddy firmware answering on {port}; use `buddy flash --full` for the first flash or recovery"
        ) from None
    parsed = parse_version(version)
    if parsed is None or parsed < LAYOUT_MIN_VERSION:
        raise FlashError(f"device runs {version or 'unknown firmware'}; flash once with `buddy flash --full`")


def default_firmware_path() -> Path:
    """The released firmware shipped in the repository (firmware/bin)."""
    root = find_repo_root()
    if root is None:
        raise FlashError("Buddy repository not found. Usage: buddy flash <firmware_dir|app.bin>")
    prebuilt = root / PREBUILT_DIR
    if not (prebuilt / "flasher_args.json").is_file():
        raise FlashError(f"{prebuilt} has no firmware; run `buddy build` and flash firmware/build")
    return prebuilt


def bundled_version() -> Optional[str]:
    """Version of the released firmware in firmware/bin, e.g. 'buddy-open 1.0.0'."""
    root = find_repo_root()
    manifest = root / PREBUILT_DIR / "manifest.json" if root else None
    if manifest is None or not manifest.is_file():
        return None
    return json.loads(manifest.read_text(encoding="utf-8")).get("version")


def export_prebuilt(build_dir: Path, out_dir: Path) -> dict:
    """Copy a build into a flat release folder with flasher_args.json and manifest.json."""
    import hashlib

    meta = json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))
    desc = json.loads((build_dir / "project_description.json").read_text(encoding="utf-8"))
    out_dir.mkdir(parents=True, exist_ok=True)
    for old in out_dir.glob("*.bin"):
        old.unlink()
    flat = {}
    for offset, rel in meta["flash_files"].items():
        name = Path(rel).name
        shutil.copyfile(build_dir / rel, out_dir / name)
        flat[offset] = name
    meta["flash_files"] = flat
    for value in meta.values():
        if isinstance(value, dict) and "file" in value:
            value["file"] = Path(value["file"]).name
    (out_dir / "flasher_args.json").write_text(json.dumps(meta, indent=4) + "\n", encoding="utf-8")
    manifest = {
        "version": f"buddy-open {desc['project_version']}",
        "chip": CHIP,
        "files": {
            name: hashlib.sha256((out_dir / name).read_bytes()).hexdigest() for name in sorted(flat.values())
        },
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=4) + "\n", encoding="utf-8")
    return manifest


def pick_flash_port(port: Optional[str]) -> str:
    if port:
        return port
    ports = find_ports()
    if not ports:
        raise FlashError(
            "no flashable USB port (303A:1001). For factory firmware or a broken app: "
            f"{PINHOLE_HINT}, then run this command again."
        )
    if len(ports) > 1:
        names = ", ".join(p.device for p in ports)
        raise FlashError(f"several 303A:1001 devices ({names}); choose one with --port")
    return ports[0].device


def _port_free(port: str) -> bool:
    from .device import _open_serial

    try:
        _open_serial(port).close()
        return True
    except Exception:  # noqa: BLE001 - cannot open means busy
        return False


def acquire_port(port: str, wait_s: float = 15.0) -> bool:
    """Make sure the port is free for flashing.

    If a buddy script holds it, ask it to release the port and wait. Returns True when a request was
    made; the caller must call portlock.clear_release() afterwards so the script can reconnect.
    """
    from . import portlock

    if _port_free(port):
        return False
    owner = portlock.current_owner(port)
    if owner is None:
        raise FlashError(
            f"{port} is in use by another program (e.g. a serial monitor). Close it and retry; "
            "buddy scripts release the port automatically, other tools do not."
        )
    print(f"{port} is used by process {owner}; asking it to release the port", flush=True)
    portlock.request_release(port)
    deadline = time.monotonic() + wait_s
    while time.monotonic() < deadline:
        time.sleep(0.5)
        if _port_free(port):
            return True
    portlock.clear_release(port)
    raise FlashError(f"process {owner} did not release {port} within {wait_s:.0f} s; stop it and retry")


def _esptool_major() -> int:
    try:
        from importlib.metadata import version

        return int(version("esptool").split(".")[0])
    except Exception:  # noqa: BLE001 - fall back to v4 syntax
        return 4


def run_esptool(port: str, plan: FlashPlan, in_download_mode: bool = False) -> int:
    """Write the plan. ``in_download_mode``: the chip already waits in ROM download mode."""
    # esptool v5 renamed commands/options to dashed forms; old forms print deprecation warnings.
    dash = _esptool_major() >= 5
    before = "no-reset" if in_download_mode else "default-reset"
    if not dash:
        before = before.replace("-", "_")
    # An RTS reset over USB-Serial-JTAG leaves the chip in download mode; a watchdog reset
    # boots the app (available since esptool v5).
    after = "watchdog-reset" if dash else "hard_reset"
    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", CHIP, "-p", port, "-b", FLASH_BAUD,
        "--before", before,
        "--after", after,
        "write-flash" if dash else "write_flash", *plan.esptool_args(),
    ]
    print("running:", " ".join(cmd), flush=True)
    return subprocess.call(cmd)


def build_firmware(root: Optional[Path] = None, extra: Optional[List[str]] = None) -> int:
    """Build into firmware/build/ (= idf.py build in firmware/). Needs an ESP-IDF environment."""
    root = root or find_repo_root()
    if root is None:
        raise FlashError("run `buddy build` inside the Buddy repository")
    fw = root / FIRMWARE_DIR
    if not (shutil.which("idf.py") or shutil.which("idf.py.exe") or os.environ.get("IDF_PATH")):
        raise FlashError(
            "ESP-IDF not found (no idf.py). Open an ESP-IDF terminal, or install ESP-IDF v6.0 (see docs/firmware.md)."
        )
    args = ["idf.py", *(extra or ["build"])]
    print("running:", " ".join(args), flush=True)
    if os.name == "nt":
        # idf.py may be a script, .bat or .exe on Windows; let the shell resolve it.
        line = " ".join(f'"{a}"' if " " in a else a for a in args)
        return subprocess.call(line, cwd=fw, shell=True)
    return subprocess.call(args, cwd=fw)
