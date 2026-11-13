# SPDX-License-Identifier: Apache-2.0
"""Pomodoro: 25 min focus -> 5 min break, repeating, starting now.

The topper jumps at every switch: 2 jumps = break starts, 3 jumps = back to focus.
The light shows the phase (red = focus, green = break). Ctrl+C stops.

Run:  python examples/pomodoro.py [focus_min] [break_min]
"""

import sys

from buddy import Buddy, BuddyCommandError

FOCUS_MIN = float(sys.argv[1]) if len(sys.argv) > 1 else 25
BREAK_MIN = float(sys.argv[2]) if len(sys.argv) > 2 else 5


def jump(b: Buddy, count: int) -> None:
    try:
        b.jump(count)
    except BuddyCommandError as exc:
        if exc.code != "busy":  # solenoid cooling down: skip the jumps this time
            raise


def main(b: Buddy) -> None:
    rounds = 0
    while True:
        b.led("red")
        jump(b, 3 if rounds else 1)
        print(f"pomodoro {rounds + 1}: focus for {FOCUS_MIN:g} min", flush=True)
        b.wait(FOCUS_MIN * 60)

        rounds += 1
        b.led("green")
        jump(b, 2)
        print(f"{rounds} done, break for {BREAK_MIN:g} min", flush=True)
        b.wait(BREAK_MIN * 60)


if __name__ == "__main__":
    with Buddy() as buddy:
        try:
            main(buddy)
        except KeyboardInterrupt:
            pass
        finally:
            buddy.led_off()
