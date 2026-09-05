# SPDX-License-Identifier: GPL-2.0-or-later
import xemu

# Replace these with addresses for the game you are investigating.
HEALTH = 0x00100000
AMMO   = 0x00100004

xemu.display_open("Game Memory HUD", 520, 220, detached=True)

# These are bound by Xemu itself. Python does not need to poll once per frame.
xemu.watch_text("health_text", HEALTH, "u32", 20, 25,
                prefix="Health: ", color="#FFFFFFFF",
                virtual=True, target="display")
xemu.watch_bar("health_bar", HEALTH, "u32", 20, 60, 460, 20,
               min_value=0, max_value=100, color="#50E080FF",
               virtual=True, target="display")
xemu.watch_text("ammo", AMMO, "hex32", 20, 105,
                prefix="Ammo raw: ", color="#FFD070FF",
                virtual=True, target="display")

# Keep the script alive while the engine updates the bound HUD elements.
while True:
    xemu.wait_frame()
