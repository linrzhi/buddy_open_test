# SPDX-License-Identifier: Apache-2.0
"""buddy command-line tool.

    buddy doctor                check the connection and suggest fixes
    buddy led red               solid red (see `buddy led -h` for blink/breathe/rainbow/off)
    buddy jump 3                jump three times
    buddy notify meeting        preset effect
    buddy remind 15:30 "Sync"   reminder at a time (keeps running until it fires)
    buddy setup                 first run: install the open firmware if needed, then check
    buddy hook done             for Claude Code / Codex hooks (never fails)
    buddy send "led 0 0 255"    raw protocol command
    buddy flash                 flash the firmware
    buddy build                 build the firmware (= idf.py build, needs ESP-IDF)

Exit codes: 0 ok, 1 device error, 2 usage, 3 not found, 4 flash/build failed.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
import time
from pathlib import Path
from typing import Any, List, Optional

from . import __version__
from .device import (
    COLORS,
    PRESETS,
    Buddy,
    BuddyCommandError,
    BuddyError,
    BuddyNotFoundError,
    find_ports,
)

EXIT_OK = 0
EXIT_DEVICE_ERROR = 1
EXIT_USAGE = 2
EXIT_NOT_FOUND = 3
EXIT_FLASH = 4

RECOVERY_HINT = "unplug USB, hold the pinhole button with a SIM pin, plug in, release, then run `buddy flash --full`"


def _setup_console() -> None:
    # Some Windows consoles default to a legacy code page; force UTF-8 output.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
        except (AttributeError, ValueError):
            pass


def _print_result(args: argparse.Namespace, payload: Any, human: str = "") -> None:
    if args.json or not human:
        print(json.dumps(payload, ensure_ascii=False))
    else:
        print(human)


def _connect(args: argparse.Namespace) -> Buddy:
    return Buddy(args.port, timeout=args.timeout)


# ------------------------------------------------------------------ commands
def cmd_ports(args: argparse.Namespace) -> int:
    ports = find_ports()
    if args.json:
        print(json.dumps([p.__dict__ for p in ports], ensure_ascii=False))
    elif not ports:
        print("no 303A:1001 serial port found")
    else:
        for p in ports:
            print(f"{p.device}\t{p.description}")
    return EXIT_OK if ports else EXIT_NOT_FOUND


def cmd_doctor(args: argparse.Namespace) -> int:
    ok = True
    print(f"buddy-cli {__version__}, Python {sys.version.split()[0]}")
    try:
        import serial  # noqa: F401
        print("[OK] pyserial installed")
    except ImportError:
        print("[X] pyserial missing: run `pip install -e .`")
        return EXIT_NOT_FOUND

    ports = find_ports()
    if not ports:
        print("[X] no Buddy serial port (303A:1001)")
        if _official_present():
            print("    The device runs the official firmware: run `buddy setup` to install the open firmware")
            return EXIT_NOT_FOUND
        print("    1. Use a data-capable USB cable and plug in directly, without a hub")
        print(f"    2. Factory firmware shows no serial port: {RECOVERY_HINT}")
        return EXIT_NOT_FOUND
    print("[OK] serial port: " + ", ".join(p.device for p in ports))

    try:
        with _connect(args) as b:
            info = b.device_info
            print(f"[OK] Buddy connected: port {b.port}, {info.get('version')}, hardware {info.get('hw')}")
            b.led("green")
            time.sleep(0.4)
            b.led("off")
            print("[OK] LED test done (short green flash)")
    except BuddyNotFoundError as exc:
        ok = False
        print(f"[X] port found but handshake failed: {exc}")
        print("    If you just flashed, replug USB once; otherwise run `buddy flash --full`")
    except BuddyError as exc:
        ok = False
        print(f"[X] device error: {exc}")
    return EXIT_OK if ok else EXIT_NOT_FOUND


def _official_present() -> bool:
    try:
        from .official import find_official

        return bool(find_official())
    except Exception:  # noqa: BLE001 - detection is best effort
        return False


def _wait_for_buddy(args: argparse.Namespace, wait_s: float = 15.0) -> Optional[dict]:
    deadline = time.monotonic() + wait_s
    while time.monotonic() < deadline:
        if args.port or find_ports():
            try:
                with Buddy(args.port, timeout=1.5) as b:
                    return b.device_info
            except BuddyError:
                pass
        time.sleep(0.5)
    return None


def cmd_setup(args: argparse.Namespace) -> int:
    """Install the released open firmware unless the device already runs it (or newer), then check."""
    from .flash import bundled_version, parse_version

    target = bundled_version()
    current = None
    if args.port or find_ports():
        try:
            with Buddy(args.port, timeout=1.5) as b:
                current = str(b.device_info.get("version", ""))
        except BuddyError:
            pass
    have, want = parse_version(current or ""), parse_version(target or "")
    if current and have and (want is None or have >= want):
        print(f"[OK] {current} is installed")
        return cmd_doctor(args)
    print(f"installing {target or 'the released firmware'}" + (f" (device runs {current})" if current else ""))
    args.firmware, args.full = None, True
    code = cmd_flash(args)
    if code != EXIT_OK:
        return code
    if _wait_for_buddy(args) is None:
        print("[X] flashed, but the device does not answer yet: replug USB and run `buddy doctor`")
        return EXIT_NOT_FOUND
    return cmd_doctor(args)


def cmd_info(args: argparse.Namespace) -> int:
    with _connect(args) as b:
        _print_result(args, b.device_info)
    return EXIT_OK


def cmd_send(args: argparse.Namespace) -> int:
    with _connect(args) as b:
        for line in args.line:
            _print_result(args, b.command(line))
    return EXIT_OK


def cmd_led(args: argparse.Namespace) -> int:
    words = [w.lower() for w in args.words]
    with _connect(args) as b:
        _print_result(args, b.command(" ".join(["led", *words])))
    return EXIT_OK


def cmd_jump(args: argparse.Namespace) -> int:
    with _connect(args) as b:
        if args.count == "stop":
            _print_result(args, b.jump_stop())
        else:
            try:
                count = int(args.count)
            except ValueError:
                print("count must be a number or `stop`", file=sys.stderr)
                return EXIT_USAGE
            _print_result(args, b.jump(count, args.duration, args.interval))
    return EXIT_OK


def _finish_notify(b: Buddy, args: argparse.Namespace) -> None:
    if args.hold > 0:
        b.wait(args.hold)  # other buddy commands can borrow the port meanwhile
        b.led_off()


def cmd_notify(args: argparse.Namespace) -> int:
    with _connect(args) as b:
        preset = b.notify(args.preset)
        if args.message and not args.json:
            print(args.message)
        _print_result(args, {"preset": args.preset, "message": args.message},
                      f"notified: {preset.description}")
        _finish_notify(b, args)
    return EXIT_OK


_DURATION_RE = re.compile(r"^(\d+)\s*(s|m|min|h)$")


def parse_when(text: str, now: Optional[dt.datetime] = None) -> dt.datetime:
    """Parse '15:30' (today, or tomorrow if past) or a relative time like '90s' / '25m' / '2h'."""
    now = now or dt.datetime.now()
    text = text.strip().lower()
    match = _DURATION_RE.match(text)
    if match:
        value, unit = int(match.group(1)), match.group(2)
        seconds = value * {"s": 1, "m": 60, "min": 60, "h": 3600}[unit]
        return now + dt.timedelta(seconds=seconds)
    try:
        hh, mm = text.split(":")
        target = now.replace(hour=int(hh), minute=int(mm), second=0, microsecond=0)
    except ValueError:
        raise ValueError("time must be HH:MM or 90s / 25m / 2h") from None
    if target <= now:
        target += dt.timedelta(days=1)
    return target


def cmd_remind(args: argparse.Namespace) -> int:
    try:
        target = parse_when(args.when)
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return EXIT_USAGE
    message = " ".join(args.message)
    print(f"reminder at {target:%Y-%m-%d %H:%M:%S}: {message or args.preset}", flush=True)
    # Check the device now rather than finding out when the time comes.
    with _connect(args):
        pass
    while True:
        remaining = (target - dt.datetime.now()).total_seconds()
        if remaining <= 0:
            break
        time.sleep(min(remaining, 30))
    with _connect(args) as b:
        b.notify(args.preset)
        print(f"⏰ {message or 'time is up'}", flush=True)
        _finish_notify(b, args)
    return EXIT_OK


def cmd_hook(args: argparse.Namespace) -> int:
    """Hook for AI coding tools. Always exits 0 so a missing device never interrupts the AI.

    - Claude Code: ``buddy hook done`` / ``buddy hook off`` (the event JSON arrives on stdin and is ignored).
    - Codex: ``notify = ["buddy", "hook"]``; Codex appends the event JSON as the last argument.
    """
    action = args.action
    if action not in PRESETS and action != "off":
        try:
            event = json.loads(action)  # Codex event JSON
        except ValueError:
            event = {}
        action = "done" if event.get("type") == "agent-turn-complete" else "info"
    try:
        with Buddy(args.port, timeout=args.timeout) as b:
            if action == "off":
                b.led_off()
            else:
                b.notify(action)
    except Exception as exc:  # noqa: BLE001 - hooks must stay silent
        print(f"[buddy hook] ignored: {exc}", file=sys.stderr)
    return EXIT_OK


def cmd_flash(args: argparse.Namespace) -> int:
    from . import portlock
    from .flash import (
        FlashError,
        acquire_port,
        check_app_only,
        default_firmware_path,
        pick_flash_port,
        plan_from_path,
        run_esptool,
    )

    from .official import SwitchError, switch_to_download

    # No serial port: the device may run the official firmware, which can switch itself to download mode.
    switch = not args.port and not find_ports()
    full = args.full or switch
    requested = False
    try:
        path = Path(args.firmware) if args.firmware else default_firmware_path()
        plan = plan_from_path(path, full=full)
        if switch:
            port = switch_to_download()
        else:
            port = pick_flash_port(args.port)
            # Keep the port borrowed from any buddy script until flashing is done.
            requested = acquire_port(port)
    except (FlashError, SwitchError) as exc:
        print(f"[X] {exc}", file=sys.stderr)
        return EXIT_FLASH
    if not full:
        try:
            check_app_only(port)
        except FlashError as exc:
            if requested:
                portlock.clear_release(port)
            print(f"[X] {exc}", file=sys.stderr)
            return EXIT_FLASH
    print(f"flashing {port}: {plan.description}", flush=True)
    try:
        code = run_esptool(port, plan, in_download_mode=switch)
    finally:
        if requested:
            portlock.clear_release(port)  # the script that lent the port reconnects
    if code != 0:
        print(f"[X] flashing failed. Try: {RECOVERY_HINT}", file=sys.stderr)
        return EXIT_FLASH
    print("[OK] flashed and restarted. Run `buddy doctor` to verify.")
    return EXIT_OK


def cmd_build(args: argparse.Namespace) -> int:
    from .flash import BUILD_DIR, PREBUILT_DIR, FlashError, build_firmware, export_prebuilt, find_repo_root

    try:
        code = build_firmware(extra=args.idf_args or None)
    except FlashError as exc:
        print(f"[X] {exc}", file=sys.stderr)
        return EXIT_FLASH
    if code != 0:
        return EXIT_FLASH
    root = find_repo_root()
    if args.export:
        manifest = export_prebuilt(root / BUILD_DIR, root / PREBUILT_DIR)
        print(f"[OK] exported {manifest['version']} to {PREBUILT_DIR.as_posix()}")
    else:
        print(f"[OK] built. Flash it with: buddy flash {BUILD_DIR.as_posix()}")
    return EXIT_OK


# ----------------------------------------------------------------- arguments
def _add_hold(p: argparse.ArgumentParser, default_hold: float) -> None:
    p.add_argument("--hold", type=float, default=default_hold,
                   help=f"seconds before the light turns off (default {default_hold:g}; 0 = stay on)")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="buddy", description="Control the Buddy desk device.",
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__,
    )
    parser.add_argument("--version", action="version", version=f"buddy-cli {__version__}")
    parser.add_argument("-p", "--port", help="serial port, e.g. COM5 or /dev/ttyACM0 (default: auto)")
    parser.add_argument("--json", action="store_true", help="JSON output for scripts and AI tools")
    parser.add_argument("--timeout", type=float, default=2.0, help="per-command timeout in seconds")
    # Global options work before or after the subcommand (buddy -p COM5 info / buddy info -p COM5).
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("-p", "--port", default=argparse.SUPPRESS, help=argparse.SUPPRESS)
    common.add_argument("--json", action="store_true", default=argparse.SUPPRESS, help=argparse.SUPPRESS)
    common.add_argument("--timeout", type=float, default=argparse.SUPPRESS, help=argparse.SUPPRESS)
    subparsers = parser.add_subparsers(dest="cmd", metavar="<command>")

    class _Sub:
        @staticmethod
        def add_parser(name: str, **kwargs: Any) -> argparse.ArgumentParser:
            return subparsers.add_parser(name, parents=[common], **kwargs)

    sub = _Sub()

    sub.add_parser("doctor", help="check the connection and suggest fixes").set_defaults(func=cmd_doctor)
    sub.add_parser("setup", help="first run: install the open firmware if needed, then check").set_defaults(func=cmd_setup)
    sub.add_parser("ports", help="list candidate serial ports").set_defaults(func=cmd_ports)
    sub.add_parser("info", help="show device info").set_defaults(func=cmd_info)

    p = sub.add_parser("send", help="send raw protocol commands")
    p.add_argument("line", nargs="+", help='e.g. "led blink red 300"')
    p.set_defaults(func=cmd_send)

    p = sub.add_parser(
        "led", help="control the LED",
        description="Colors: " + " ".join(COLORS) + " or R G B (0-255).\n"
        "Examples: buddy led red | buddy led 255 120 0 | buddy led blink blue 300 | "
        "buddy led breathe green 2000 | buddy led rainbow | buddy led off | buddy led (query)",
    )
    p.add_argument("words", nargs="*")
    p.set_defaults(func=cmd_led)

    p = sub.add_parser("jump", help="solenoid jumps: buddy jump [count|stop] [duration_ms] [interval_ms]")
    p.add_argument("count", nargs="?", default="1")
    p.add_argument("duration", nargs="?", type=int, default=80)
    p.add_argument("interval", nargs="?", type=int, default=150)
    p.set_defaults(func=cmd_jump)

    preset_help = "; ".join(f"{k} = {v.description}" for k, v in PRESETS.items())
    p = sub.add_parser("notify", help="run a preset effect", description=preset_help)
    p.add_argument("preset", nargs="?", default="info", choices=list(PRESETS))
    p.add_argument("-m", "--message", default="", help="text to print as well")
    _add_hold(p, default_hold=0)
    p.set_defaults(func=cmd_notify)

    p = sub.add_parser("remind", help='reminder: buddy remind 15:30 "Sync" / buddy remind 25m "Break"')
    p.add_argument("when", help="HH:MM, or 90s / 25m / 2h")
    p.add_argument("message", nargs="*")
    p.add_argument("--preset", default="meeting", choices=list(PRESETS))
    _add_hold(p, default_hold=60)
    p.set_defaults(func=cmd_remind)

    p = sub.add_parser("hook", help="AI tool hook: buddy hook done|off|<preset>|<Codex event JSON>; never fails")
    p.add_argument("action", nargs="?", default="done")
    p.set_defaults(func=cmd_hook)

    p = sub.add_parser("flash", help="flash firmware (default: released firmware in firmware/bin, app only)")
    p.add_argument("firmware", nargs="?", help="firmware directory (with flasher_args.json), app .bin, or merged .bin with --full")
    p.add_argument("--full", action="store_true", help="write the complete image; for the first flash and recovery")
    p.set_defaults(func=cmd_flash)

    p = sub.add_parser("build", help="build firmware into firmware/build (= idf.py build, needs ESP-IDF)")
    p.add_argument("--export", action="store_true", help="also update the released firmware in firmware/bin")
    p.add_argument("idf_args", nargs=argparse.REMAINDER, help="arguments for idf.py (default: build)")
    p.set_defaults(func=cmd_build)
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    _setup_console()
    parser = build_parser()
    args = parser.parse_args(argv)
    if not getattr(args, "func", None):
        parser.print_help()
        return EXIT_USAGE
    try:
        return args.func(args)
    except BuddyNotFoundError as exc:
        print(f"[X] {exc}", file=sys.stderr)
        return EXIT_NOT_FOUND
    except BuddyCommandError as exc:
        if args.json:
            print(json.dumps({"error": exc.code, "message": exc.message}, ensure_ascii=False))
        else:
            print(f"[X] device refused: {exc.code} {exc.message}", file=sys.stderr)
        return EXIT_DEVICE_ERROR
    except (BuddyError, TimeoutError) as exc:
        print(f"[X] {exc}", file=sys.stderr)
        return EXIT_DEVICE_ERROR
    except ValueError as exc:
        print(f"[X] {exc}", file=sys.stderr)
        return EXIT_USAGE
    except ImportError as exc:
        print(f"[X] missing dependency {exc.name}: run `pip install -e .` in the repository folder", file=sys.stderr)
        return EXIT_NOT_FOUND
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
