import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// The 0.99 compare inside FUN_00a87150 was redirected and the flip survived,
// so the LiSPSM-vs-uniform decision is made by a CALLER. Decompile every
// caller of the three projection builders (a87150 LiSPSM, a88b10, a88200)
// and of the frame function a5d600, fully, so the branch can be read.
public class LispsmCallers extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/lispsm_callers.txt", "UTF-8");
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        Set<String> done = new HashSet<>();
        for (long a : new long[]{0x00a87150L, 0x00a88b10L, 0x00a88200L, 0x00a86d50L}) {
            Function f = getFunctionAt(toAddr(a));
            out.println("======== callers of " + f.getName() + " ========");
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                Function cf = getFunctionContaining(r.getFromAddress());
                if (cf == null) { out.println("  " + r.getFromAddress() + " (no function)"); continue; }
                out.println("  " + r.getFromAddress() + " in " + cf.getName());
                if (cf.getName().startsWith("thunk")) continue;
                if (!done.add(cf.getName())) continue;
                DecompileResults dr = dec.decompileFunction(cf, 240, new ConsoleTaskMonitor());
                String c = (dr != null && dr.getDecompiledFunction() != null) ? dr.getDecompiledFunction().getC() : "(failed)";
                out.println("################ " + cf.getName() + " @ " + cf.getEntryPoint() + " size=" + cf.getBody().getNumAddresses() + " ################");
                out.println(c.length() > 60000 ? c.substring(0, 60000) : c);
                out.println();
                for (Reference r2 : getReferencesTo(cf.getEntryPoint())) {
                    Function c2 = getFunctionContaining(r2.getFromAddress());
                    out.println("    <- called from " + r2.getFromAddress() + " in " + (c2 == null ? "?" : c2.getName()));
                }
            }
        }
        out.close();
        println("wrote lispsm_callers.txt");
    }
}
