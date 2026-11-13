# apps

Put your own Buddy scripts here (for example `apps/stand_up.py`).
Everything in this folder except this README is ignored by git, so `git pull` never conflicts with your apps.

```python
from buddy import Buddy

with Buddy() as b:
    b.notify("water")
    b.wait(60)
    b.led_off()
```

Run with `python apps/<name>.py`. See [../examples](../examples) and [../docs/recipes.md](../docs/recipes.md).
