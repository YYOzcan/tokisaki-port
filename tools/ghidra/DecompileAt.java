// Tokisaki Port -- decompile the functions at the given addresses.
//   analyzeHeadless out/ghidra KZ2 -process EBOOT.ELF -noanalysis \
//       -scriptPath tools/ghidra -postScript DecompileAt.java 0x2B6460 0x3AC15C
// (tools/decompile.sh wraps this.)
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (String a : getScriptArgs()) {
            Address addr = toAddr(Long.parseLong(a.replace("0x", ""), 16));
            Function f = getFunctionAt(addr);
            if (f == null) {
                disassemble(addr);
                f = createFunction(addr, null);
            }
            if (f == null) f = getFunctionContaining(addr);
            if (f == null) { println("==== " + a + " : no function"); continue; }
            DecompileResults r = di.decompileFunction(f, 180, monitor);
            println("==== " + a + " " + f.getName() + " @" + f.getEntryPoint());
            println(r.getDecompiledFunction() != null ? r.getDecompiledFunction().getC()
                                                      : "DECOMPILE FAILED: " + r.getErrorMessage());
        }
    }
}
