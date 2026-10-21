# Show the level script timers (OnUpdate ...) and how often scripts run.
import gdb, time
gdb.execute("set pagination off"); gdb.execute("set confirm off")
gdb.execute("handle all nostop noprint pass")
T0 = time.time()
def vb(): return int(gdb.parse_and_eval("*(unsigned char**)&vm_base"))
def gpr(i):
    rdi = int(gdb.parse_and_eval("$rdi"))
    return int(gdb.parse_and_eval("*(unsigned long*)(%d)" % (rdi + 8 * i))) & 0xFFFFFFFF
def rd32(ea): return int.from_bytes(bytes(gdb.selected_inferior().read_memory(vb() + ea, 4)), "big")
def cstr(ea, n=64):
    b = bytes(gdb.selected_inferior().read_memory(vb() + ea, n)); return b.split(b"\0")[0].decode("latin1")
class B(gdb.Breakpoint):
    def __init__(self, sym, tag, dump):
        super().__init__(sym, internal=True); self.silent = True; self.tag = tag; self.n = 0; self.last = -100; self.lastn = 0; self.dump = dump
    def stop(self):
        self.n += 1
        t = time.time() - T0
        if t - self.last >= 10:
            msg = "[scr] t=%.0f %s n=%d (+%d)" % (t, self.tag, self.n, self.n - self.lastn)
            if self.dump:
                lst = gpr(3); cnt = rd32(lst); arr = rd32(lst + 4)
                ents = []
                for i in range(min(cnt, 12)):
                    e = arr + 20 * i
                    ref = rd32(e)
                    ents.append("[%s f=%d int=%.2f cd=%.2f]" % (cstr(ref + 0x10, 40) if ref else "-", rd32(e + 4) >> 24,
                        __import__("struct").unpack(">f", rd32(e + 8).to_bytes(4, "big"))[0],
                        __import__("struct").unpack(">f", rd32(e + 12).to_bytes(4, "big"))[0]))
                msg += " timers=%d %s" % (cnt, " ".join(ents))
            print(msg, flush=True)
            self.last = t; self.lastn = self.n
        return False
B("func_002CEA30", "TimerTick", True)
B("func_003E5A10", "ScriptCall", False)
gdb.execute("run")
