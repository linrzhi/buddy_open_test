# Recipes

Use them as-is, or paste the "Ask the AI" line into Claude Code / Codex and let it adapt the idea.

## 1. Jump when the AI is done

The light turns green with two jumps when the AI finishes, blinks blue when it needs your approval,
and turns off when you send a new message.

### Claude Code

Add to `~/.claude/settings.json` (or `.claude/settings.json` for one project):

```json
{
  "hooks": {
    "Stop": [
      { "hooks": [{ "type": "command", "command": "buddy hook done" }] }
    ],
    "Notification": [
      { "hooks": [{ "type": "command", "command": "buddy hook meeting" }] }
    ],
    "UserPromptSubmit": [
      { "hooks": [{ "type": "command", "command": "buddy hook off" }] }
    ]
  }
}
```

### Codex

Add to `~/.codex/config.toml`:

```toml
notify = ["buddy", "hook"]
```

Codex calls it after each turn with an event JSON argument; Buddy lights green and jumps twice.

`buddy hook` exits successfully even when the device is unplugged, so it never interrupts the AI.

> Ask the AI: "Set up Buddy to notify me when you finish a task and turn off when I send a new message."

## 2. Daily reminders

Script: [examples/daily_reminders.py](../examples/daily_reminders.py). Edit the `REMINDERS` list.

```bash
python examples/daily_reminders.py
```

One-off reminders need no script:

```bash
buddy remind 15:30 "Team sync"                    # 3 jumps, light on for 60 s
buddy remind 40m "Oven" --preset alert --hold 300 # 3 quick jumps, light on for 5 min
```

> Ask the AI: "Remind me to stretch at 9:30 and 14:00 on weekdays and to write my weekly report on Friday 17:00.
> Start it at login and tell me how to stop it."

## 3. Calendar reminders

The computer reads the calendar:

- **Outlook (Windows):** `pywin32`
- **Google / other calendars:** an iCal (.ics) URL parsed with `icalendar`
- **macOS Calendar:** AppleScript or `icalBuddy`

> Ask the AI: "Read my Outlook calendar and blink blue with three jumps 5 minutes before each meeting."

## 4. Pomodoro

Script: [examples/pomodoro.py](../examples/pomodoro.py)

```bash
python examples/pomodoro.py          # 25 + 5 minutes
python examples/pomodoro.py 50 10    # 50 + 10 minutes
```

Jumps at every switch: 2 = break starts, 3 = back to focus. The light shows the phase (red = focus, green = break).
Ctrl+C to stop.

## 5. Monitoring alerts (CI, websites, prices…)

Jumps only when the state changes: 3 quick jumps and a blinking red light when it fails, 2 jumps when it recovers.

```python
import urllib.request
from buddy import Buddy

URL = "https://example.com"


def site_ok() -> bool:
    try:
        urllib.request.urlopen(URL, timeout=10)
        return True
    except Exception:
        return False


with Buddy() as b:
    was_ok = True
    while True:
        ok = site_ok()
        if ok != was_ok:
            b.notify("done" if ok else "alert")   # alert keeps the red light blinking
            if ok:
                b.wait(10)
                b.led_off()
            was_ok = ok
        b.wait(60)
```

> Ask the AI: "Check the latest CI run of my GitHub repo every 10 minutes; jump three times when it fails
> and twice when it recovers."

## 6. Start at login

- **Windows:** Task Scheduler → trigger "At log on" → action `pythonw.exe <script>`
- **macOS:** a plist in `~/Library/LaunchAgents/`
- **Linux:** a `systemd --user` service

> Ask the AI: "Run daily_reminders.py in the background at login without a console window, and tell me how to stop it."

## Tips

- Time, network and calendars are handled on the computer. Jumps are the main signal; the light shows a state.
- End reminders with `--hold` or `b.wait()`.
- In long-running scripts use `b.wait()` instead of `time.sleep()` so other `buddy` commands can borrow the port.
- Keep jumping moderate (20% average duty). On a `busy` error, wait or use the light only.
