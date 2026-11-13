# SPDX-License-Identifier: Apache-2.0
"""Client for the Buddy serial protocol (firmware: components/buddy_cmd).

- The host sends one command per line.
- The device answers each command with one line: ``OK`` / ``OK {json}`` / ``ERR <code> <message>``.
- Device events are ``EVT {json}`` and may appear between any lines.
- Any other line (e.g. ROM output on reset) is ignored.
"""

from __future__ import annotations

import json
import sys
import time
from collections import deque
from dataclasses import dataclass
from typing import Any, Callable, Deque, Dict, Iterator, List, Optional, Sequence, Tuple, Union

from . import portlock

# Default VID:PID of the chip's built-in USB-Serial-JTAG (same in app and ROM download mode).
USB_JTAG_VID = 0x303A
USB_JTAG_PID = 0x1001

DEFAULT_TIMEOUT = 2.0

# `info` reports {"version": "buddy-open <x.y.z>", "hw": "REVn"}.
VERSION_PREFIX = "buddy-open "

COLORS = ("red", "green", "blue", "white", "yellow", "orange", "cyan", "purple", "pink")

Color = Union[str, Tuple[int, int, int], Sequence[int]]


@dataclass(frozen=True)
class Preset:
    """A notification effect: light + jumps."""

    led_mode: str  # constant / blink / breathe / rainbow
    color: str
    led_ms: Optional[int]
    jumps: int
    jump_ms: int = 80
    jump_gap_ms: int = 150
    description: str = ""


PRESETS: Dict[str, Preset] = {
    "done": Preset("constant", "green", None, 2, description="task done: green + 2 jumps"),
    "meeting": Preset("blink", "blue", 500, 3, description="meeting/event: blue blink + 3 jumps"),
    "alert": Preset("blink", "red", 200, 3, 60, 100, description="alert/error: fast red blink + 3 quick jumps"),
    "water": Preset("breathe", "cyan", 2000, 1, description="drink/break: cyan breathe + 1 jump"),
    "pomodoro": Preset("breathe", "orange", 1500, 2, description="pomodoro: orange breathe + 2 jumps"),
    "info": Preset("constant", "white", None, 1, description="info: white + 1 jump"),
    "party": Preset("rainbow", "white", None, 4, 60, 120, description="celebrate: rainbow + 4 jumps"),
}


class BuddyError(Exception):
    """Base class for Buddy errors."""


class BuddyNotFoundError(BuddyError):
    """No device found, or the device is not running the Buddy firmware."""


class _PortLost(BuddyError):
    """The port disappeared while reading."""


class BuddyCommandError(BuddyError):
    """The device replied with ERR."""

    def __init__(self, code: str, message: str, command: str = "") -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message
        self.command = command


@dataclass(frozen=True)
class PortInfo:
    device: str
    vid: Optional[int]
    pid: Optional[int]
    description: str


def find_ports() -> List[PortInfo]:
    """List serial ports that may be a Buddy (VID:PID 303A:1001)."""
    from serial.tools import list_ports

    result = []
    for p in list_ports.comports():
        if p.vid == USB_JTAG_VID and p.pid == USB_JTAG_PID:
            result.append(PortInfo(p.device, p.vid, p.pid, p.description or ""))
    return result


def _open_serial(port: str) -> Any:
    """Open the port without resetting the chip.

    USB-Serial-JTAG treats DTR/RTS changes as reset / download-mode requests,
    so both must be False before open().
    """
    import serial

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200  # ignored by USB-Serial-JTAG
    ser.timeout = 0.05
    ser.write_timeout = 1.0
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def parse_reply(line: str) -> Tuple[str, Any]:
    """Parse one line into (kind, payload); kind is ``ok`` / ``err`` / ``evt`` / ``other``."""
    if line == "OK":
        return "ok", {}
    if line.startswith("OK "):
        body = line[3:].strip()
        try:
            return "ok", json.loads(body)
        except ValueError:
            return "ok", {"text": body}
    if line.startswith("ERR "):
        parts = line[4:].split(" ", 1)
        return "err", (parts[0], parts[1] if len(parts) > 1 else "")
    if line.startswith("EVT "):
        try:
            return "evt", json.loads(line[4:])
        except ValueError:
            return "other", line
    return "other", line


def _color_args(color: Color) -> List[str]:
    if isinstance(color, str):
        name = color.strip().lower()
        if name not in COLORS:
            raise ValueError(f"unknown color {color!r}; use one of {', '.join(COLORS)} or (r, g, b)")
        return [name]
    values = list(color)
    if len(values) != 3 or any(not isinstance(v, int) or not 0 <= v <= 255 for v in values):
        raise ValueError("RGB color must be three integers 0-255")
    return [str(v) for v in values]


class Buddy:
    """Connection to one Buddy device.

    ``Buddy()`` finds the device automatically; ``Buddy("COM5")`` uses a given port.
    Use ``with Buddy() as b:`` so the port is closed on exit.
    """

    def __init__(
        self,
        port: Optional[str] = None,
        timeout: float = DEFAULT_TIMEOUT,
        *,
        serial_factory: Callable[[str], Any] = _open_serial,
        connect: bool = True,
        share_port: Optional[bool] = None,
    ) -> None:
        self.port = port
        self.timeout = timeout
        self._serial_factory = serial_factory
        self._ser: Any = None
        self._rx = bytearray()
        self._events: Deque[Dict[str, Any]] = deque()
        self.device_info: Dict[str, Any] = {}
        # Only real ports take part in port sharing (test doubles write no lock files).
        # share_port=False: a short-lived client that must not lend the port (used by `buddy flash`).
        self._cooperate = (serial_factory is _open_serial) if share_port is None else share_port
        self._borrowed_port: Optional[str] = None
        if connect:
            self.connect()

    # ------------------------------------------------------------ connection
    def connect(self) -> Dict[str, Any]:
        """Open the port, handshake and return ``info``."""
        if self.port:
            candidates = [self.port]
        else:
            candidates = [p.device for p in find_ports()]
            if not candidates:
                raise BuddyNotFoundError(
                    "no Buddy found (USB 303A:1001). Check the cable; "
                    "a device with factory firmware needs `buddy flash --full` first."
                )

        errors = []
        for port in candidates:
            try:
                self._ser = self._open_port(port)
            except Exception as exc:  # noqa: BLE001 - pyserial raises many types
                errors.append(f"{port}: cannot open ({exc}); another program may be using it")
                continue
            try:
                info = self._handshake()
            except BuddyError as exc:
                errors.append(f"{port}: {exc}")
                self.close()
                continue
            self.port = port
            self.device_info = info
            if self._cooperate:
                portlock.register_owner(port)
            return info

        raise BuddyNotFoundError("; ".join(errors))

    def _open_port(self, port: str) -> Any:
        """Open the port; if another buddy program holds it, ask it to lend the port."""
        try:
            return self._serial_factory(port)
        except Exception:
            owner = portlock.current_owner(port) if self._cooperate else None
            if owner is None:
                raise
        portlock.request_release(port)
        self._borrowed_port = port
        deadline = time.monotonic() + 10.0
        while True:
            time.sleep(0.3)
            try:
                return self._serial_factory(port)
            except Exception:
                if time.monotonic() >= deadline:
                    portlock.clear_release(port)
                    self._borrowed_port = None
                    raise

    def _handshake(self) -> Dict[str, Any]:
        self._ser.reset_input_buffer()
        self._rx.clear()
        # An empty line clears any partial input left on the device.
        self._write_line("")
        try:
            info = self.command("info", timeout=1.5)
        except TimeoutError:
            raise BuddyNotFoundError(
                "no response; the device may be in download mode or not running the Buddy firmware. "
                "Run `buddy flash --full`."
            ) from None
        if not str(info.get("version", "")).startswith(VERSION_PREFIX):
            raise BuddyNotFoundError(f"not a Buddy device: {info}")
        return info

    def close(self) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            finally:
                self._ser = None
                if self._cooperate and self.port:
                    portlock.unregister_owner(self.port)
        if self._borrowed_port:
            # Done borrowing; the owner reconnects by itself.
            portlock.clear_release(self._borrowed_port)
            self._borrowed_port = None

    def _reconnect(self, timeout: float = 60.0) -> None:
        """Wait for the device to come back (reset, flashed) and handshake again."""
        self.close()
        deadline = time.monotonic() + timeout
        last_error: Optional[Exception] = None
        while time.monotonic() < deadline:
            try:
                self.connect()
                return
            except BuddyError as exc:
                last_error = exc
                time.sleep(1.0)
        raise BuddyNotFoundError(f"device did not come back within {timeout:.0f} s: {last_error}")

    def _yield_if_requested(self) -> None:
        """Release the port while another program needs it, then reconnect."""
        if not (self._cooperate and self.port and portlock.release_requested(self.port)):
            return
        if self._borrowed_port == self.port:
            return  # our own request while borrowing
        if portlock.release_stale(self.port):
            portlock.clear_release(self.port)
            return
        print(f"[buddy] lending {self.port} to another program; will reconnect afterwards",
              file=sys.stderr, flush=True)
        self.close()
        while portlock.release_requested(self.port) and not portlock.release_stale(self.port):
            time.sleep(0.5)
        self._reconnect()
        print(f"[buddy] reconnected to {self.port} (firmware {self.device_info.get('version')})",
              file=sys.stderr, flush=True)

    def __enter__(self) -> "Buddy":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # --------------------------------------------------------------- raw I/O
    def _write_line(self, line: str) -> None:
        if self._ser is None:
            raise BuddyError("port is not open")
        self._ser.write((line + "\n").encode("ascii"))
        self._ser.flush()

    def _read_line(self, deadline: float) -> Optional[str]:
        while True:
            idx = self._rx.find(b"\n")
            if idx >= 0:
                raw = bytes(self._rx[:idx])
                del self._rx[: idx + 1]
                return raw.decode("utf-8", "replace").strip("\r\x00 ")
            if time.monotonic() >= deadline:
                return None
            try:
                chunk = self._ser.read(256)
            except Exception as exc:  # noqa: BLE001 - pyserial raises many types on unplug/reset
                raise _PortLost(f"port lost: {exc}") from exc
            if chunk:
                self._rx.extend(chunk)

    def command(self, line: str, timeout: Optional[float] = None) -> Dict[str, Any]:
        """Send one raw command and return the JSON after OK ({} if none).

        Raises BuddyCommandError on ERR and TimeoutError if no reply arrives.
        """
        line = line.strip()
        if "\n" in line or "\r" in line:
            raise ValueError("send one command line at a time")
        self._yield_if_requested()
        self._write_line(line)
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while True:
            text = self._read_line(deadline)
            if text is None:
                raise TimeoutError(f"no reply to {line!r}")
            kind, payload = parse_reply(text)
            if kind == "ok":
                return payload
            if kind == "err":
                raise BuddyCommandError(payload[0], payload[1], line)
            if kind == "evt":
                self._events.append(payload)

    def events(self, timeout: Optional[float] = None) -> Iterator[Dict[str, Any]]:
        """Yield device events (currently only ``boot``); timeout=None waits forever."""
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            while self._events:
                yield self._events.popleft()
            remaining = 0.5 if deadline is None else deadline - time.monotonic()
            if remaining <= 0:
                return
            self._yield_if_requested()
            try:
                text = self._read_line(time.monotonic() + min(remaining, 0.5))
            except _PortLost:
                print("[buddy] port lost, waiting for the device to come back", file=sys.stderr, flush=True)
                self._reconnect()
                continue
            if text is None:
                continue
            kind, payload = parse_reply(text)
            if kind == "evt":
                yield payload

    # ------------------------------------------------------------- high level
    def ping(self) -> bool:
        return bool(self.command("ping").get("pong"))

    def info(self) -> Dict[str, Any]:
        return self.command("info")

    def help(self) -> List[str]:
        return list(self.command("help").get("commands", []))

    def led(self, color: Color = "white", mode: str = "constant", ms: Optional[int] = None) -> Dict[str, Any]:
        """Control the RGB LED.

        - ``led("red")`` solid; ``led((255, 100, 0))`` custom color
        - ``led("blue", "blink", 300)`` blink, toggling every 300 ms (1-60000)
        - ``led("green", "breathe", 2000)`` breathe with a 2 s period (100-60000)
        - ``led("off")`` off; ``led(mode="rainbow")`` rainbow
        """
        if isinstance(color, str) and color.lower() == "off":
            return self.command("led off")
        if mode == "rainbow":
            return self.command("led rainbow")
        if mode not in ("constant", "blink", "breathe"):
            raise ValueError("mode must be constant / blink / breathe / rainbow")
        parts = ["led"]
        if mode != "constant":
            parts.append(mode)
        parts += _color_args(color)
        if ms is not None:
            if mode == "constant":
                raise ValueError("ms is not used in constant mode")
            parts.append(str(int(ms)))
        return self.command(" ".join(parts))

    def led_off(self) -> Dict[str, Any]:
        return self.command("led off")

    def jump(self, count: int = 1, duration_ms: int = 80, interval_ms: int = 150) -> Dict[str, Any]:
        """Jump ``count`` times. Too much jumping returns a ``busy`` error (thermal budget)."""
        return self.command(f"jump {int(count)} {int(duration_ms)} {int(interval_ms)}")

    def jump_stop(self) -> Dict[str, Any]:
        return self.command("jump stop")

    def wait(self, seconds: float) -> None:
        """Wait ``seconds``; use instead of time.sleep().

        While waiting, other buddy commands and ``buddy flash`` can borrow the port.
        """
        for _ in self.events(timeout=seconds):
            pass

    def notify(self, preset: str = "info") -> Preset:
        """Run a preset effect (see PRESETS). The light stays on until changed."""
        try:
            p = PRESETS[preset]
        except KeyError:
            raise ValueError(f"unknown preset {preset!r}; use one of {', '.join(PRESETS)}") from None
        if p.led_mode == "rainbow":
            self.led(mode="rainbow")
        else:
            self.led(p.color, p.led_mode, p.led_ms)
        if p.jumps:
            self.jump(p.jumps, p.jump_ms, p.jump_gap_ms)
        return p

    def reboot(self) -> None:
        """Restart the device. The port closes; create a new Buddy() afterwards."""
        try:
            self.command("reboot", timeout=1.0)
        finally:
            self.close()
