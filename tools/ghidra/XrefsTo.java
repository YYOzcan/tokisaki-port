// Tokisaki Port -- list the references to the given addresses and the
// functions they sit in.
//   analyzeHeadless out/ghidra KZ2 -process EBOOT.ELF.0 -noanalysis -readOnly \
//       -scriptPath tools/ghidra -postScript XrefsTo.java 0xBF1D54
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class XrefsTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String a : getScriptArgs()) {
            Address addr = toAddr(Long.parseLong(a.replace("0x", ""), 16));
            println("==== refs to " + addr);
            for (Reference r : getReferencesTo(addr)) {
                Function f = getFunctionContaining(r.getFromAddress());
                println("  from " + r.getFromAddress() + " " + r.getReferenceType() + " in "
                        + (f != null ? f.getName() + " @" + f.getEntryPoint() : "?"));
            }
        }
    }
}
