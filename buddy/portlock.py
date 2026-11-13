# SPDX-License-Identifier: Apache-2.0
"""Port sharing: long-running scripts lend the serial port to other programs.

Only one program can open USB-Serial-JTAG at a time. A reminder script keeps the port open,
which would block esptool and other commands. Coordination uses small files in the temp dir:

- ``<port>.owner``: pid of the process holding the port;
- ``<port>.release``: created by a program that needs the port. The holder closes the port,
  waits until the file is removed, then reconnects.

Only programs using this library cooperate; other serial tools must be closed manually.
"""

from __future__ import annotations

import os
import re
import tempfile
import time
from pathlib import Path
from typing import Optional

LOCK_DIR = Path(tempfile.gettempdir()) / "buddy-cli"


def _key(port: str) -> str:
    return re.sub(r"[^A-Za-z0-9]", "_", port)


def owner_file(port: str) -> Path:
    return LOCK_DIR / f"{_key(port)}.owner"


def release_file(port: str) -> Path:
    return LOCK_DIR / f"{_key(port)}.release"


def pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if os.name == "nt":
        # On Windows os.kill(pid, 0) terminates the process; query with OpenProcess instead.
        import ctypes

        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            return False
        try:
            code = ctypes.c_ulong()
            if not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
                return False
            return code.value == STILL_ACTIVE
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def register_owner(port: str) -> None:
    try:
        LOCK_DIR.mkdir(parents=True, exist_ok=True)
        owner_file(port).write_text(str(os.getpid()), encoding="ascii")
    except OSError:
        pass


def unregister_owner(port: str) -> None:
    path = owner_file(port)
    try:
        if path.read_text(encoding="ascii").strip() == str(os.getpid()):
            path.unlink()
    except (OSError, ValueError):
        pass


def current_owner(port: str) -> Optional[int]:
    """Return the pid of a live holder, or None."""
    try:
        pid = int(owner_file(port).read_text(encoding="ascii").strip())
    except (OSError, ValueError):
        return None
    if pid == os.getpid() or not pid_alive(pid):
        return None
    return pid


def release_requested(port: str) -> bool:
    return release_file(port).exists()


def request_release(port: str) -> None:
    LOCK_DIR.mkdir(parents=True, exist_ok=True)
    release_file(port).write_text(str(os.getpid()), encoding="ascii")


def clear_release(port: str) -> None:
    try:
        release_file(port).unlink()
    except OSError:
        pass


def release_stale(port: str, max_age_s: float = 600.0) -> bool:
    """A request file older than max_age_s (left by a crashed flasher) is stale."""
    try:
        return time.time() - release_file(port).stat().st_mtime > max_age_s
    except OSError:
        return False
