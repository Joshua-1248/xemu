local xemu = require("xemu")

xemu.display_open("Lua Advanced HUD", 760, 430, true)
xemu.overlay_set("title", 18, 18, "Lua HUD", "#FFFFFFFF", 1.3, "#000000B0")
xemu.controller_show("pad", 1, 20, 70, 1.0, "overlay", true)
xemu.controller_show("pad_ext", 1, 30, 150, 1.3, "display", true)
xemu.rect("panel", 15, 55, 260, 165, "#80B0FFFF", false, 2.0, 8.0, "overlay")
xemu.crosshair("center", 400, 215, 18, "#FF6060FF", 2.0, 5.0, true, "display")
xemu.line("divider", 360, 20, 360, 400, "#FFFFFF60", 1.0, "display")
xemu.circle("radar", 560, 210, 85, "#60FFB0FF", false, 2.0, "display")

for i=1,1800 do
  local f = xemu.wait_frame()
  local value = 50 + 50 * math.sin(f / 50)
  xemu.bar("activity", 385, 60, 300, 22, value, 0, 100,
           "#46C864FF", "#000000A0", "#FFFFFF80", false, "display")
  xemu.display_text("frame", 385, 95, "Frame: "..f, "#FFFFFFFF", 1.2)
  xemu.display_text("state", 385, 120, "Run state: "..xemu.runstate())
end
