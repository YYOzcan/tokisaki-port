# Find the code that reads the right stick: at the first pad-state update
# (func_0038A8A8) after WARM seconds, put hardware read watchpoints on port
# 0's right-stick floats (raw x,y at +0x18C, filtered x,y at +0x194) and print
# every distinct host backtrace that reads them.
#   WARM=200 PAD_SCRIPT=... tools/gdb/run_gdb.sh tools/gdb/stick_readers.py
import gdb, os, time
gdb.execute("set pagination off"); gdb.execute("set confirm off")
gdb.execute("handle all nostop noprint pass")
T0 = time.time(); WARM = float(os.environ.get("WARM", "200"))
seen = {}
class W(gdb.Breakpoint):
    def __init__(self, expr, tag):
        super().__init__(expr, gdb.BP_WATCHPOINT, gdb.WP_READ, internal=True)
        self.silent = True; self.tag = tag
    def stop(self):
        names = []; f = gdb.newest_frame()
        while f is not None and len(names) < 10:
            names.append(f.name() or hex(f.pc())); f = f.older()
        key = (self.tag,) + tuple(names[:6]); seen[key] = seen.get(key, 0) + 1
        if seen[key] in (1, 100, 1000):
            print("[rw] t=%.0f %s #%d %s" % (time.time() - T0, self.tag, seen[key], " <- ".join(names)), flush=True)
        return False
class B(gdb.Breakpoint):
    def stop(self):
        return time.time() - T0 >= WARM
b = B("*_Z13func_0038A8A8P11ppu_context", internal=True)
gdb.execute("run")
ctx = int(gdb.parse_and_eval("(unsigned long long)$rdi"))
p = int(gdb.parse_and_eval("*(unsigned long long*)%d" % (ctx + 3 * 8))) & 0xffffffff
base = int(gdb.parse_and_eval("*(unsigned long long*)&vm_base"))
print("[arm] t=%.0f param_1=0x%x vm_base=0x%x" % (time.time() - T0, p, base), flush=True)
b.enabled = False
W("*(long long*)0x%x" % (base + p + 0x18C), "raw")
W("*(long long*)0x%x" % (base + p + 0x194), "filt")
gdb.execute("continue")
