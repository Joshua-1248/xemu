# Xemu custom-fork Python scripting text-display example.
import xemu

xemu.overlay_set("hello", 20, 55, "Python internal overlay",
                 color="#FFFFFFFF", scale=1.25,
                 background="#000000A0")

xemu.display_open("Python External Display", 720, 360, detached=True)
xemu.display_text("title", 20, 20, "Python external display",
                  color="#FFD060FF", scale=1.5)

try:
    while True:
        frame = xemu.wait_frame()
        xemu.overlay_set("frame", 20, 90, f"Frame: {frame}",
                         color="#80E0FFFF")
        xemu.display_text("frame", 20, 65, f"Frame: {frame}",
                          color="#80E0FFFF")
except (KeyboardInterrupt, EOFError):
    pass
finally:
    xemu.overlay_clear()
    xemu.display_clear()
