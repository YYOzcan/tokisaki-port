# Killzone 2 look path probe: at every call of the player aim update
# (func_0016E620) after WARM seconds, print the aim object's outputs of the
# previous frame (+0x2F0/+0x2F4 look out, +0x2F8/+0x2FC rates) with the frame
# dt (0xF2A1D4) and port 0's right stick (input manager 0x117E740); after
# WATCH_AT seconds put a read watchpoint on +0x2F0 to find who consumes it.
import gdb, os, struct, time
gdb.execute("set pagination off"); gdb.execute("set confirm off")
gdb.execute("handle all nostop noprint pass")
T0 = time.time(); WARM = float(os.environ.get("WARM", "195"))
WATCH_AT = float(os.environ.get("WATCH_AT", "1e9"))
inf = gdb.selected_inferior(); base = [0]; objs = {}; seen = {}
def rf(a):
    return struct.unpack(">f", bytes(inf.read_memory(base[0] + a, 4)))[0]
def ru(a):
    return struct.unpack(">I", bytes(inf.read_memory(base[0] + a, 4)))[0]
class W(gdb.Breakpoint):
    def __init__(self, expr, tag):
        super().__init__(expr, gdb.BP_WATCHPOINT, gdb.WP_READ, internal=True)
        self.silent = True; self.tag = tag
    def stop(self):
        names = []; f = gdb.newest_frame()
        while f is not None and len(names) < 9:
            names.append(f.name() or hex(f.pc())); f = f.older()
        key = tuple(names[:5]); seen[key] = seen.get(key, 0) + 1
        if seen[key] in (1, 100):
            print("[rw] t=%.0f %s #%d %s" % (time.time() - T0, self.tag, seen[key], " <- ".join(names)), flush=True)
        return False
class B(gdb.Breakpoint):
    def stop(self):
        t = time.time() - T0
        if t < WARM: return False
        if not base[0]:
            base[0] = int(gdb.parse_and_eval("*(unsigned long long*)&vm_base"))
        ctx = int(gdb.parse_and_eval("(unsigned long long)$rdi"))
        o = int(gdb.parse_and_eval("*(unsigned long long*)%d" % (ctx + 3 * 8))) & 0xffffffff
        objs[o] = objs.get(o, 0) + 1
        if objs[o] == 1: print("[obj] t=%.0f 0x%x" % (t, o), flush=True)
        first = min(objs, key=lambda k: -objs[k])
        if o == first:
            print("[look] t=%.2f dt=%.4f rx=%.3f ry=%.3f out=%.5f,%.5f rate=%.5f,%.5f a78=%.5f aF8=%.5f a74=%.5f aF4=%.5f aFC=%.5f" % (
                t, rf(0xF2A1D4), rf(0x117E740 + 4 + 0x20), rf(0x117E740 + 4 + 0x24),
                rf(o + 0x2F0), rf(o + 0x2F4), rf(o + 0x2F8), rf(o + 0x2FC),
                rf(o + 0x78), rf(o + 0xF8), rf(o + 0x74), rf(o + 0xF4), rf(o + 0xFC)), flush=True)
            if t >= WATCH_AT and not getattr(self, "w", None):
                self.w = W("*(long long*)0x%x" % (base[0] + o + 0x2F0), "+0x2F0")
        return False
B("*_Z13func_0016E620P11ppu_context", internal=True)
gdb.execute("run")
