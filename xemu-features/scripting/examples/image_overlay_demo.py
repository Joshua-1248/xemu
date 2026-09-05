# SPDX-License-Identifier: GPL-2.0-or-later
import os
import xemu

# image() accepts PNG/JPEG/BMP/etc. formats supported by Xemu's stb_image.
# Relative paths are resolved relative to this script file's directory.
image_path = os.path.join(os.path.dirname(__file__), "example_overlay.png")
if os.path.exists(image_path):
    xemu.image("logo", image_path, 20, 20, 256, 128,
               tint="#FFFFFFFF", target="display")
else:
    xemu.display_open("Image Overlay Demo", 500, 240, detached=True)
    xemu.display_text("missing", 20, 20,
        "Place example_overlay.png beside this script to test image drawing.",
        "#FFD080FF", 1.0, "#000000A0")
