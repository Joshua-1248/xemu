import xemu

if not xemu.debug_available():
    raise RuntimeError("Enable the Debug Tools feature to use this example")

# Fill in addresses from the Disassembler.
EXEC_ADDRESS = 0x00100000
WATCH_ADDRESS = 0x00101000

xemu.breakpoint_add(EXEC_ADDRESS)
xemu.watchpoint_add(WATCH_ADDRESS, 4, "rw")

def show_event(ev):
    r = xemu.regs()
    xemu.overlay_set("debug_stop", 20, 20,
        f"{ev['type']} @ EIP={r.get('eip', 0):08X}  addr={ev['address']:08X}",
        "#FF8080FF", 1.1, "#000000C0")
    print(ev)
    print(r)

# Wait for any guest-debug stop. The callback runs while Xemu is paused, so
# you can safely inspect registers/memory, patch data, then resume.
while True:
    ev = xemu.wait_debug_event()
    show_event(ev)
    xemu.resume()
