# SPDX-License-Identifier: Apache-2.0
"""Unit tests for the buddy client, using a fake serial port (no hardware needed).

Run: python -m unittest discover -s tests
"""

import datetime as dt
import json
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from buddy import Buddy, BuddyCommandError, BuddyNotFoundError  # noqa: E402
from buddy.cli import build_parser, parse_when  # noqa: E402
from buddy.device import parse_reply  # noqa: E402


class FakeSerial:
    """Fake serial port that answers commands line by line; can inject boot noise and events."""

    def __init__(self, replies=None, noise=b"", device="buddy-open"):
        self.replies = replies or {}
        self.device = device
        self.out = bytearray(noise)
        self.written = []
        self.closed = False

    # subset of the pyserial API
    def reset_input_buffer(self):
        pass

    def write(self, data):
        for line in data.decode().splitlines() or [""]:
            self.written.append(line)
            self._respond(line.strip())

    def flush(self):
        pass

    def read(self, n):
        chunk = bytes(self.out[:n])
        del self.out[:n]
        return chunk

    def close(self):
        self.closed = True

    def push(self, text):
        self.out.extend(text.encode())

    def _respond(self, line):
        if not line:
            return
        if line in self.replies:
            self.push(self.replies[line])
        elif line == "info":
            self.push('OK {"version":"%s 1.0.0","hw":"REV1"}\r\n' % self.device)
        elif line.startswith(("led", "jump", "ping")):
            self.push("OK {}\r\n")


def make(fake):
    return Buddy("FAKE", timeout=0.3, serial_factory=lambda _port: fake)


class ParseReplyTest(unittest.TestCase):
    def test_kinds(self):
        self.assertEqual(parse_reply("OK"), ("ok", {}))
        self.assertEqual(parse_reply('OK {"a":1}'), ("ok", {"a": 1}))
        self.assertEqual(parse_reply("ERR busy cooling down"), ("err", ("busy", "cooling down")))
        self.assertEqual(parse_reply('EVT {"event":"boot"}'), ("evt", {"event": "boot"}))
        self.assertEqual(parse_reply("ESP-ROM:esp32s3")[0], "other")


class BuddyTest(unittest.TestCase):
    def test_handshake_ignores_boot_noise(self):
        fake = FakeSerial(noise=b"ESP-ROM:esp32s3-20210327\r\nrst:0x15\r\n")
        b = make(fake)
        self.assertEqual(b.device_info, {"version": "buddy-open 1.0.0", "hw": "REV1"})
        self.assertEqual(fake.written[:2], ["", "info"])

    def test_rejects_non_buddy(self):
        with self.assertRaises(BuddyNotFoundError):
            make(FakeSerial(device="other"))

    def test_silent_port_is_not_found(self):
        fake = FakeSerial(replies={"info": ""})
        with self.assertRaises(BuddyNotFoundError):
            make(fake)
        self.assertTrue(fake.closed)

    def test_led_commands(self):
        fake = FakeSerial()
        b = make(fake)
        b.led("Red")
        b.led((1, 2, 3), "blink", 300)
        b.led("green", "breathe")
        b.led("off")
        b.led(mode="rainbow")
        self.assertEqual(
            fake.written[2:],
            ["led red", "led blink 1 2 3 300", "led breathe green", "led off", "led rainbow"],
        )
        with self.assertRaises(ValueError):
            b.led("magenta")
        with self.assertRaises(ValueError):
            b.led("red", ms=100)

    def test_err_raises_with_code(self):
        fake = FakeSerial(replies={"jump 9 80 150": "ERR busy solenoid cooling down, retry in 3 s\r\n"})
        b = make(fake)
        with self.assertRaises(BuddyCommandError) as ctx:
            b.jump(9)
        self.assertEqual(ctx.exception.code, "busy")

    def test_events_interleaved_with_reply_are_kept(self):
        fake = FakeSerial(replies={"ping": 'EVT {"event":"boot"}\r\nOK {"pong":true}\r\n'})
        b = make(fake)
        self.assertTrue(b.ping())
        self.assertEqual(list(b.events(timeout=0.2)), [{"event": "boot"}])

    def test_wait_returns_after_timeout(self):
        b = make(FakeSerial())
        start = time.monotonic()
        b.wait(0.3)
        self.assertGreaterEqual(time.monotonic() - start, 0.25)

    def test_notify_preset(self):
        fake = FakeSerial()
        make(fake).notify("meeting")
        self.assertEqual(fake.written[2:], ["led blink blue 500", "jump 3 80 150"])


class CliTest(unittest.TestCase):
    def test_global_options_after_subcommand(self):
        args = build_parser().parse_args(["flash", "build", "--full", "-p", "COM14"])
        self.assertEqual(args.port, "COM14")
        self.assertTrue(args.full)
        self.assertEqual(build_parser().parse_args(["setup"]).func.__name__, "cmd_setup")
        args = build_parser().parse_args(["-p", "COM3", "--json", "info"])
        self.assertEqual(args.port, "COM3")
        self.assertTrue(args.json)

    def test_parse_when(self):
        now = dt.datetime(2026, 11, 10, 15, 0, 0)
        self.assertEqual(parse_when("25m", now), now + dt.timedelta(minutes=25))
        self.assertEqual(parse_when("15:30", now), now.replace(minute=30))
        self.assertEqual(parse_when("09:00", now), dt.datetime(2026, 11, 11, 9, 0))
        with self.assertRaises(ValueError):
            parse_when("soon", now)


class FlashPlanTest(unittest.TestCase):
    def _build_dir(self, tmp):
        files = {"0x0": "bootloader.bin", "0x8000": "partition-table.bin", "0x10000": "buddy.bin"}
        for name in files.values():
            (tmp / name).write_bytes(b"\0")
        meta = {"flash_files": files, "app": {"offset": "0x10000", "file": "buddy.bin"}}
        (tmp / "flasher_args.json").write_text(json.dumps(meta), encoding="utf-8")

    def test_plans(self):
        import tempfile

        from buddy.flash import DATA_AREA_OFFSET, DATA_AREA_SIZE, plan_from_path

        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            self._build_dir(tmp)
            app = plan_from_path(tmp)
            self.assertEqual([(o, p.name) for o, p in app.images], [(0x10000, "buddy.bin")])
            full = plan_from_path(tmp, full=True)
            self.assertEqual([o for o, _ in full.images], [0x0, 0x8000, DATA_AREA_OFFSET, 0x10000])
            blank = dict(full.images)[DATA_AREA_OFFSET].read_bytes()
            self.assertEqual(blank, b"\xff" * DATA_AREA_SIZE)
            self.assertEqual(DATA_AREA_OFFSET + DATA_AREA_SIZE, 0x10000)

    def test_merged_image(self):
        import tempfile

        from buddy.flash import FlashError, plan_from_path

        with tempfile.TemporaryDirectory() as d:
            merged = Path(d) / "official.bin"
            merged.write_bytes(bytes([0xE9]) + bytes([0xFF]) * 0x7FFF + bytes([0xAA, 0x50]) + bytes([0xFF]) * 16)
            self.assertEqual([o for o, _ in plan_from_path(merged, full=True).images], [0x0])
            app = Path(d) / "app.bin"
            app.write_bytes(bytes([0xE9]) + bytes(0x9000))
            with self.assertRaises(FlashError):
                plan_from_path(app, full=True)
            self.assertEqual([o for o, _ in plan_from_path(app).images], [0x10000])

    def test_released_firmware_in_repo(self):
        from buddy.flash import DEFAULT_APP_OFFSET, default_firmware_path, plan_from_path

        prebuilt = default_firmware_path()
        self.assertEqual(prebuilt.parts[-2:], ("firmware", "bin"))
        manifest = json.loads((prebuilt / "manifest.json").read_text(encoding="utf-8"))
        self.assertTrue(manifest["version"].startswith("buddy-open "))
        offsets = [o for o, _ in plan_from_path(prebuilt, full=True).images]
        self.assertIn(DEFAULT_APP_OFFSET, offsets)

    def test_parse_version(self):
        from buddy.flash import LAYOUT_MIN_VERSION, parse_version

        self.assertEqual(parse_version("buddy-open 1.0.0"), (1, 0, 0))
        self.assertLess(parse_version("buddy-open 0.9.0"), LAYOUT_MIN_VERSION)
        self.assertIsNone(parse_version("buddy-open x"))


class FakeHid:
    """Minimal stand-in for the hidapi module."""

    def __init__(self, feature, release=0x0101):
        self.feature = feature
        self.release = release
        self.sent = []

    def enumerate(self, vid, pid):
        return [
            {"path": b"a", "usage_page": 0xFF00, "usage": 1, "release_number": self.release},
            {"path": b"b", "usage_page": 0x000C, "usage": 1, "release_number": self.release},
        ]

    def device(self):
        fake = self

        class Handle:
            def open_path(self, path):
                assert path == b"a"

            def get_feature_report(self, report_id, length):
                if fake.feature is None:
                    raise OSError("no such report")
                return list(fake.feature)

            def send_feature_report(self, data):
                fake.sent.append(bytes(data))

            def close(self):
                pass

        return Handle()


class OfficialTest(unittest.TestCase):
    def _patch(self, fake):
        from unittest import mock

        from buddy import official

        return mock.patch.object(official, "_hid", lambda: fake)

    def test_detect_and_request(self):
        from unittest import mock

        from buddy import official

        fake = FakeHid(bytes([0xD0, 0x42, 0x44, 0x59, 0x21, 0x01, 0x01, 0x00]))
        with self._patch(fake):
            devices = official.find_official()
            self.assertEqual([d.path for d in devices], [b"a"])
            self.assertTrue(official.supports_download(devices[0]))
            calls = iter([[], [], [mock.Mock(device="COM9")]])
            with mock.patch.object(official, "find_ports", lambda: next(calls)),                     mock.patch.object(official.time, "sleep", lambda _s: None):
                self.assertEqual(official.switch_to_download(), "COM9")
        self.assertEqual(fake.sent, [bytes([0xD0, 0x42, 0x44, 0x59, 0x21, 0x01, 0x01, 0x00])])

    def test_old_firmware_is_refused(self):
        from buddy import official

        for feature in (None, bytes([0xD0, 0, 0, 0, 0, 1, 1, 0]), bytes([0xD0, 0x42, 0x44, 0x59, 0x21, 0x01, 0x00, 0x00])):
            fake = FakeHid(feature)
            with self._patch(fake):
                with self.assertRaises(official.SwitchError):
                    official.switch_to_download()
            self.assertEqual(fake.sent, [])

    def test_without_hidapi(self):
        from buddy import official

        with self._patch(None):
            self.assertEqual(official.find_official(), [])
            with self.assertRaises(official.SwitchError):
                official.switch_to_download()


if __name__ == "__main__":
    unittest.main()
