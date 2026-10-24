# Access watchpoint on one 4-byte field of the aim object (func_0016E620's r3):
# print each distinct accessor backtrace and, per access, the field's value.
#   FIELD=0xF8 WARM=200 ... tools/gdb/run_gdb.sh "$PWD/tools/gdb/field_watch.py"
import gdb, os, struct, time
gdb.execute("set pagination off"); gdb.execute("set confirm off")
gdb.execute("handle all nostop noprint pass")
T0 = time.time(); WARM = float(os.environ.get("WARM", "200"))
FIELD = int(os.environ.get("FIELD", "0xF8"), 16); FUNC = int(os.environ.get("FUNC", "0x16E620"), 16)
NPRINT = int(os.environ.get("NPRINT", "40"))
inf = gdb.selected_inferior(); seen = {}; st = {"n": 0}
class W(gdb.Breakpoint):
    def __init__(self, addr):
        super().__init__("*(int*)0x%x" % addr, gdb.BP_WATCHPOINT, gdb.WP_ACCESS, internal=True)
        self.silent = True; self.addr = addr
    def stop(self):
        names = []; f = gdb.newest_frame()
        while f is not None and len(names) < 8:
            names.append(f.name() or hex(f.pc())); f = f.older()
        v = struct.unpack(">f", bytes(inf.read_memory(self.addr, 4)))[0]
        key = tuple(names[:4]); seen[key] = seen.get(key, 0) + 1
        st["n"] += 1
        if seen[key] == 1 or st["n"] <= NPRINT:
            print("[acc] t=%.2f v=%.5f #%d %s" % (time.time() - T0, v, seen[key], " <- ".join(names)), flush=True)
        return False
class B(gdb.Breakpoint):
    def stop(self):
        return time.time() - T0 >= WARM
b = B("*_Z13func_%08XP11ppu_context" % FUNC, internal=True)
gdb.execute("run")
base = int(gdb.parse_and_eval("*(unsigned long long*)&vm_base"))
ctx = int(gdb.parse_and_eval("(unsigned long long)$rdi"))
o = int(gdb.parse_and_eval("*(unsigned long long*)%d" % (ctx + 3 * 8))) & 0xffffffff
print("[arm] obj=0x%x field=0x%x" % (o, FIELD), flush=True)
b.enabled = False
W(base + o + FIELD)
gdb.execute("continue")
