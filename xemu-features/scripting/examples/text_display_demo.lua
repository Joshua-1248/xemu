-- Xemu custom-fork Lua scripting text-display example.
local xemu = require("xemu")

xemu.overlay_set("hello", 20, 55, "Lua internal overlay",
                 "#FFFFFFFF", 1.25, "#000000A0")

xemu.display_open("Lua External Display", 720, 360, true)
xemu.display_text("title", 20, 20, "Lua external display",
                  "#FFD060FF", 1.5)

while true do
  local frame = xemu.wait_frame()
  xemu.overlay_set("frame", 20, 90, "Frame: " .. frame,
                   "#80E0FFFF")
  xemu.display_text("frame", 20, 65, "Frame: " .. frame,
                    "#80E0FFFF")
end
