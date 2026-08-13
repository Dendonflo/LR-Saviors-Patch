import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// The Gysahl clobber writes 0x017FE0A8 into the script's plot-name stack slot.
// That resolves to Ghidra 0x00D9E0A8 - an address in .text, preceded by an
// Unwind@ symbol, whose body is the classic stack-cookie/unwind funclet shape.
// It is referenced as DATA by exactly two functions:
//     FUN_009e3b70 and FUN_009e3670
// Both sit immediately around FUN_009e34c0, the script-coroutine YIELD
// primitive behind White.delay() / drawSync / sleep. If the yield path sets up
// an exception frame over the script's locals, that explains why the buffer
// dies specifically during suspension - and why frame rate matters (how many
// yields happen while the box is open).
//
// Decompile all three plus the yield, and list what calls them.
public class YieldNeighbours extends GhidraScript {
    private void callers(PrintWriter out, String addr) throws Exception {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        Function f = getFunctionContaining(a);
        if (f == null) return;
        out.println("  callers of " + f.getName() + ":");
        int n = 0;
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            Function c = getFunctionContaining(r.getFromAddress());
            out.println("    " + r.getFromAddress() + " " + r.getReferenceType()
                        + (c == null ? "" : "  in " + c.getName() + " @ " + c.getEntryPoint()));
            if (++n > 25) { out.println("    ..."); break; }
        }
        if (n == 0) out.println("    (none)");
    }

    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/yield_neighbours.txt","UTF-8");
        DecompInterface d = new DecompInterface();
        DecompileOptions o = new DecompileOptions();
        o.setMaxPayloadMBytes(128);
        d.setOptions(o);
        d.openProgram(currentProgram);

        String[][] t = {
            {"009e34c0", "SCRIPT YIELD primitive (White.delay/drawSync/sleep)"},
            {"009e3670", "references the unwind handler (DATA)"},
            {"009e3b70", "references the unwind handler (DATA)"},
        };
        for (String[] x : t) {
            Address a = currentProgram.getAddressFactory().getAddress(x[0]);
            Function f = getFunctionContaining(a);
            if (f == null) { out.println("no function at " + x[0]); continue; }
            out.println("======== " + x[1] + "\n   " + f.getName() + " @ " + f.getEntryPoint()
                        + " size=" + f.getBody().getNumAddresses() + " ========");
            DecompileResults dr = d.decompileFunction(f, 600, new ConsoleTaskMonitor());
            out.println(dr != null && dr.decompileCompleted()
                        ? dr.getDecompiledFunction().getC() : "FAILED");
            callers(out, x[0]);
            out.println();
            out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
