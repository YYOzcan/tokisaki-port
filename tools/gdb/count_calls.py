# Count calls to lifted guest functions and print the first ones with r4
# (for a type's create function, r4 is the new object). Killzone 2: the
# sequence types' create functions from the reflection table (PROGRESS 36).
import gdb, time
gdb.execute("set pagination off"); gdb.execute("set confirm off")
gdb.execute("handle all nostop noprint pass")
def gpr(i):
    rdi = int(gdb.parse_and_eval("$rdi"))
    return int(gdb.parse_and_eval("*(unsigned long*)(%d)" % (rdi + 8 * i))) & 0xFFFFFFFF
T0 = time.time()
class C(gdb.Breakpoint):
    def __init__(self, sym, tag):
        super().__init__(sym, internal=True); self.silent = True; self.tag = tag; self.n = 0
    def stop(self):
        self.n += 1
        if self.n <= 60:
            print("[seq] t=%.0f %s #%d r4=0x%08X" % (time.time() - T0, self.tag, self.n, gpr(4)), flush=True)
        return False
C("func_001D4A20", "Sequence")
C("func_001D4BC8", "SequenceRep")
C("func_001D4E28", "SequenceCameraEntity")
C("func_001D4EA0", "SequenceCameraEntityRep")
C("func_001C71A0", "StartSequenceEventInstance")
C("func_001C7060", "SkipSequenceEventInstance")
C("func_001C72B0", "StopSequenceEventInstance")
gdb.execute("run")
