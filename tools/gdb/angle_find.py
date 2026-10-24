# Find the camera yaw/pitch: while PAD_STICK holds the right stick (the look
# delta per frame is the aim object's +0x2F0), snapshot +-RANGE bytes around
# the aim object and its owner at consecutive calls of func_0016E620 and list
# the floats whose per-frame change matches the look delta.
import gdb, os, struct, time
gdb.execute("set pagination off"); gdb.execute("set confirm off")
gdb.execute("handle all nostop noprint pass")
T0 = time.time(); AT = float(os.environ.get("AT", "203")); RANGE = int(os.environ.get("RANGE", "0x100000"), 16)
inf = gdb.selected_inferior(); st = {"n": 0, "snaps": []}
def rf(base, a): return struct.unpack(">f", bytes(inf.read_memory(base + a, 4)))[0]
class B(gdb.Breakpoint):
    def stop(self):
        t = time.time() - T0
        if t < AT or st["n"] >= 4: return False
        base = int(gdb.parse_and_eval("*(unsigned long long*)&vm_base"))
        ctx = int(gdb.parse_and_eval("(unsigned long long)$rdi"))
        o = int(gdb.parse_and_eval("*(unsigned long long*)%d" % (ctx + 3 * 8))) & 0xffffffff
        if st.get("o") not in (None, o): return False
        st["o"] = o
        pl = struct.unpack(">I", bytes(inf.read_memory(base + o + 0x10, 4)))[0]
        regs = []
        for c in (o, pl):
            lo = max(0x10000, (c - RANGE) & ~0xF); regs.append((lo, bytes(inf.read_memory(base + lo, 2 * RANGE))))
        st["snaps"].append((t, rf(base, o + 0x2F0), rf(base, o + 0x2F4), regs))
        st["n"] += 1
        print("[snap] t=%.2f obj=0x%x player=0x%x out=%.5f" % (t, o, pl, rf(base, o + 0x2F0)), flush=True)
        if st["n"] == 4:
            report()
        return False
def report():
    s = st["snaps"]
    for ri in range(2):
        lo = s[0][3][ri][0]; n = len(s[0][3][ri][1]) // 4
        arrs = [struct.unpack(">%df" % n, x[3][ri][1]) for x in s]
        hits = 0
        for i in range(n):
            v = [a[i] for a in arrs]
            if any(x != x or abs(x) > 1e6 for x in v): continue
            d = [v[k + 1] - v[k] for k in range(3)]
            if os.environ.get("TRIG"):
                ok = all(abs(x) <= 1.001 for x in v) and all(abs(x) > 0.002 for x in d)
            else:
                ok = all(0.004 < abs(x) < 0.05 for x in d) and max(d) - min(d) < 0.05 * max(abs(x) for x in d) + 1e-5
            if ok:
                print("[cand] region%d 0x%08x vals %s deltas %s" % (ri, lo + 4 * i,
                      ",".join("%.4f" % x for x in v), ",".join("%.5f" % x for x in d)), flush=True)
                hits += 1
                if hits > 150: break
    print("[done]", flush=True)
B("*_Z13func_0016E620P11ppu_context", internal=True)
gdb.execute("run")
