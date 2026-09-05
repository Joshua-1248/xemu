# SPDX-License-Identifier: GPL-2.0-or-later
import math
import xemu

# Internal overlay + external native debug canvas.
xemu.display_open("Python Advanced HUD", 760, 430, detached=True)
xemu.overlay_set("title", 18, 18, "Python HUD", "#FFFFFFFF", 1.3, "#000000B0")
xemu.controller_show("pad", port=1, x=20, y=70, scale=1.0, target="overlay")
xemu.controller_show("pad_ext", port=1, x=30, y=150, scale=1.3, target="display")

# Free-form primitives.
xemu.rect("panel", 15, 55, 260, 165, "#80B0FFFF", False, 2.0, 8.0)
xemu.crosshair("center", 400, 215, 18, "#FF6060FF", 2.0, 5.0, True, "display")
xemu.line("divider", 360, 20, 360, 400, "#FFFFFF60", 1.0, "display")
xemu.circle("radar", 560, 210, 85, "#60FFB0FF", False, 2.0, "display")

# A fake live gauge driven by frame count. In a game-specific script you can
# replace this with watch_bar() and bind it directly to guest RAM.
for _ in range(1800):
    f = xemu.wait_frame()
    value = 50.0 + 50.0 * math.sin(f / 50.0)
    xemu.bar("activity", 385, 60, 300, 22, value, 0, 100,
             "#46C864FF", "#000000A0", "#FFFFFF80", False, "display")
    xemu.display_text("frame", 385, 95, f"Frame: {f}", "#FFFFFFFF", 1.2)
    xemu.display_text("state", 385, 120, f"Run state: {xemu.runstate()}")
