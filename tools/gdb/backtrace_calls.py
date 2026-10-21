# Print host backtraces (lifted guest frames) for calls to a guest function.
import gdb, time
gdb.execute("set pagination off"); gdb.execute("set confirm off")
gdb.execute("handle all nostop noprint pass")
T0 = time.time()
class B(gdb.Breakpoint):
    def __init__(self, sym, tag, nmax):
        super().__init__(sym, internal=True); self.silent = True; self.tag = tag; self.n = 0; self.nmax = nmax
    def stop(self):
        self.n += 1
        if self.n in (1, 50, 2000) or (self.n <= self.nmax):
            print("[bt] t=%.0f %s #%d" % (time.time() - T0, self.tag, self.n), flush=True)
            try:
                f = gdb.newest_frame(); k = 0
                while f is not None and k < 14:
                    nm = f.name() or "?"
                    print("   ", nm, flush=True); f = f.older(); k += 1
            except Exception as e:
                print("bt fail", e)
        if self.n % 5000 == 0:
            print("[bt] t=%.0f %s count=%d" % (time.time() - T0, self.tag, self.n), flush=True)
        return False
B("func_0066DED8", "EntityBase.Update", 1)
gdb.execute("run")
