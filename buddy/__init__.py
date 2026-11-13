# SPDX-License-Identifier: Apache-2.0
"""Python library for the Buddy desk device.

Usage::

    from buddy import Buddy

    with Buddy() as b:          # finds the device automatically
        b.led("blue")           # blue light
        b.jump(2)               # two jumps
        b.wait(60)              # wait a minute (instead of time.sleep)
        b.led("off")
"""

from .device import (
    Buddy,
    BuddyCommandError,
    BuddyError,
    BuddyNotFoundError,
    COLORS,
    PRESETS,
    find_ports,
)

__all__ = [
    "Buddy",
    "BuddyCommandError",
    "BuddyError",
    "BuddyNotFoundError",
    "COLORS",
    "PRESETS",
    "find_ports",
]

__version__ = "1.0.0"
