# SPDX-License-Identifier: Apache-2.0
"""Daily reminders: the topper jumps (with a light) at set times.

Run:  python examples/daily_reminders.py
Edit REMINDERS below. The computer provides the time, so it must stay on.
"""

import datetime as dt
import time
from typing import Optional

from buddy import Buddy, BuddyCommandError

# (time, text, preset, weekdays only?)
# presets (light + jumps): done=2 meeting=3 alert=3 quick water=1 pomodoro=2 info=1 party=4
REMINDERS = [
    ("09:30", "Stand up and stretch", "water", True),
    ("12:00", "Lunch", "info", False),
    ("14:00", "Team sync", "meeting", True),
    ("18:00", "Wrap up the day", "party", True),
]

# Seconds the light stays on after a reminder.
LIGHT_SECONDS = 60


def due_today(now: dt.datetime, hhmm: str, weekdays_only: bool) -> Optional[dt.datetime]:
    hh, mm = map(int, hhmm.split(":"))
    target = now.replace(hour=hh, minute=mm, second=0, microsecond=0)
    if weekdays_only and target.weekday() >= 5:
        return None
    return target


def remind(b: Buddy, text: str, preset: str) -> None:
    print(f"{dt.datetime.now():%H:%M} {text}", flush=True)
    try:
        b.notify(preset)
    except BuddyCommandError as exc:
        if exc.code != "busy":  # solenoid cooling down: light only this time
            raise


def main() -> None:
    fired = set()  # (date, time, text) already done today
    light_off_at = None
    with Buddy() as b:
        print(f"connected to {b.port}, {len(REMINDERS)} reminders, Ctrl+C to quit", flush=True)
        try:
            while True:
                now = dt.datetime.now()
                for hhmm, text, preset, weekdays_only in REMINDERS:
                    target = due_today(now, hhmm, weekdays_only)
                    key = (now.date(), hhmm, text)
                    # A 2-minute window tolerates sleep or a busy computer.
                    if target and target <= now < target + dt.timedelta(minutes=2) and key not in fired:
                        fired.add(key)
                        remind(b, text, preset)
                        light_off_at = time.monotonic() + LIGHT_SECONDS
                        b.wait(3)  # let this effect finish before the next reminder
                if light_off_at and time.monotonic() >= light_off_at:
                    b.led_off()
                    light_off_at = None
                b.wait(5)  # not time.sleep: lets other buddy commands borrow the port
        finally:
            b.led_off()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
