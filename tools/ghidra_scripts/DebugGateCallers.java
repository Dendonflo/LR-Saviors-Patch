import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// FUN_00ccea50 - the DebugComponentEnabled predicate - is compiled to
// "return 0" in retail. Decompile every caller to see exactly what a forced
// "return 1" would light up (and what might crash because content was
// stripped). FUN_0042e890 (AppGame ctor, 800 lines) is already dumped in
// debug_menu_survey.txt; skip it here to keep this file readable, but list
// its call sites.
public class DebugGateCallers extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/debug_gate_callers.txt", "UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        Address gate = toAddr(0xccea50L);
        Map<Address, List<Address>> callers = new LinkedHashMap<>();
        for (Reference r : getReferencesTo(gate)) {
            Function f = getFunctionContaining(r.getFromAddress());
            if (f == null) { out.println("ref outside function: " + r.getFromAddress()); continue; }
            callers.computeIfAbsent(f.getEntryPoint(), k -> new ArrayList<>()).add(r.getFromAddress());
        }
        out.println("=== " + callers.size() + " distinct callers of FUN_00ccea50 ===");
        for (Map.Entry<Address, List<Address>> e : callers.entrySet()) {
            Function f = getFunctionAt(e.getKey());
            out.println("  " + f.getName() + " @ " + e.getKey() + "  sites: " + e.getValue());
        }
        out.println();
        for (Address entry : callers.keySet()) {
            if (entry.getOffset() == 0x42e890L) {
                out.println("### FUN_0042e890 skipped (in debug_menu_survey.txt)");
                continue;
            }
            Function f = getFunctionAt(entry);
            out.println("############################################################");
            out.println("### " + f.getName() + " @ " + entry);
            out.print("### its callers:");
            int n = 0;
            for (Reference r : getReferencesTo(entry)) {
                Function cf = getFunctionContaining(r.getFromAddress());
                if (cf != null) { out.print(" " + cf.getName()); if (++n > 8) { out.print(" ..."); break; } }
            }
            out.println();
            DecompileResults res = dec.decompileFunction(f, 120, new ConsoleTaskMonitor());
            if (res != null && res.decompileCompleted())
                out.println(res.getDecompiledFunction().getC());
            else out.println("  (decompile failed)");
        }
        out.close();
        println("done");
    }
}
