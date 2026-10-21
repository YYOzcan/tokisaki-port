# Log luaL_error / luaG_runerror / luaD_seterrorobj in the game's Lua 5.1.
import gdb
gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("handle all nostop noprint pass")

def vb():
    return int(gdb.parse_and_eval("*(unsigned char**)&vm_base"))

def gpr(i):
    rdi = int(gdb.parse_and_eval("$rdi"))
    return int(gdb.parse_and_eval("*(unsigned long*)(%d)" % (rdi + 8 * i))) & 0xFFFFFFFF

def cstr(ea, n=200):
    if not ea or ea >= 0xF0000000:
        return "<0x%08X>" % ea
    try:
        b = bytes(gdb.selected_inferior().read_memory(vb() + ea, n))
    except Exception:
        return "<bad 0x%08X>" % ea
    return b.split(b"\0")[0].decode("latin1")

class Log(gdb.Breakpoint):
    def __init__(self, sym, tag, fmt_reg):
        super().__init__(sym, internal=True)
        self.tag = tag; self.fmt_reg = fmt_reg; self.silent = True; self.n = 0
    def stop(self):
        self.n += 1
        if self.n <= 400:
            f = cstr(gpr(self.fmt_reg))
            a = [gpr(r) for r in range(self.fmt_reg + 1, self.fmt_reg + 4)]
            s = [cstr(x, 160) if 0x10000 <= x < 0x60000000 else "0x%X" % x for x in a]
            print("[lua] %s #%d fmt=%r args=%r lr=0x%08X" % (self.tag, self.n, f, s, gpr(-1) if False else 0), flush=True)
        return False

Log("func_003F3978", "luaL_error", 4)
Log("func_003EFD40", "luaG_runerror", 4)

def rd32(ea):
    b = bytes(gdb.selected_inferior().read_memory(vb() + ea, 4))
    return int.from_bytes(b, "big")

class ErrObj(gdb.Breakpoint):
    def __init__(self):
        super().__init__("func_003E6790", internal=True)
        self.silent = True; self.n = 0
    def stop(self):
        self.n += 1
        if self.n <= 400:
            L = gpr(3); code = gpr(4)
            top = rd32(L + 8)
            v, tt = rd32(top - 8), rd32(top - 4)
            msg = cstr(v + 0x10, 300) if tt == 4 else "tt=%d v=0x%X" % (tt, v)
            print("[lua] seterrorobj #%d L=0x%08X code=%d msg=%r" % (self.n, L, code, msg), flush=True)
        return False
ErrObj()
gdb.execute("run")
