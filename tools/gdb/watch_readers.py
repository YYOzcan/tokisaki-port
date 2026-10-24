# Who reads a guest object's fields: at the first call of guest function FUNC
# after WARM seconds, take its r3 (or REG) as the object, put hardware read
# watchpoints (8 bytes each, at most 4) at the offsets in WATCH and print every
# distinct host backtrace that reads them.
#   FUNC=0x00AFC370 WATCH=0x34,0x2C WARM=190 PAD_SCRIPT=... \
#     tools/gdb/run_gdb.sh "$PWD/tools/gdb/watch_readers.py"
import gdb, os, time
gdb.execute("set pagination off"); gdb.execute("set confirm off")
gdb.execute("handle all nostop noprint pass")
T0 = time.time(); WARM = float(os.environ.get("WARM", "190"))
FUNC = "_Z13func_%08XP11ppu_context" % int(os.environ["FUNC"], 16)
REG = int(os.environ.get("REG", "3"))
OFFS = [int(x, 16) for x in os.environ.get("WATCH", "0").split(",")]
DEPTH = int(os.environ.get("DEPTH", "8"))
seen = {}
class W(gdb.Breakpoint):
    def __init__(self, expr, tag):
        super().__init__(expr, gdb.BP_WATCHPOINT, gdb.WP_READ, internal=True)
        self.silent = True; self.tag = tag
    def stop(self):
        names = []; f = gdb.newest_frame()
        while f is not None and len(names) < DEPTH:
            names.append(f.name() or hex(f.pc())); f = f.older()
        key = (self.tag,) + tuple(names[:5]); seen[key] = seen.get(key, 0) + 1
        if seen[key] in (1, 100, 1000):
            print("[rw] t=%.0f %s #%d %s" % (time.time() - T0, self.tag, seen[key], " <- ".join(names)), flush=True)
        return False
class B(gdb.Breakpoint):
    def stop(self):
        return time.time() - T0 >= WARM
b = B("*" + FUNC, internal=True)
gdb.execute("run")
ctx = int(gdb.parse_and_eval("(unsigned long long)$rdi"))
p = int(gdb.parse_and_eval("*(unsigned long long*)%d" % (ctx + REG * 8))) & 0xffffffff
base = int(gdb.parse_and_eval("*(unsigned long long*)&vm_base"))
print("[arm] t=%.0f obj=0x%x vm_base=0x%x" % (time.time() - T0, p, base), flush=True)
b.enabled = False
for o in OFFS[:4]:
    W("*(long long*)0x%x" % (base + p + o), "+0x%X" % o)
gdb.execute("continue")
