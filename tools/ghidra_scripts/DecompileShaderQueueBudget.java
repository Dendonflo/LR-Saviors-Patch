import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Set;

public class DecompileShaderQueueBudget extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/shader_queue_budget.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        String[] targets = {"00aa7420", "00aa7500", "00aa7530", "00aa77c0", "00aa7630", "00ab7740"};
        for (String t : targets) {
            Address addr = currentProgram.getAddressFactory().getAddress(t);
            Function func = getFunctionAt(addr);
            out.println("=== FUN_" + t + " ===");
            if (func == null) { out.println("  not found"); out.println(); continue; }
            DecompileResults res = decomp.decompileFunction(func, 60, new ConsoleTaskMonitor());
            if (res != null && res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("  decompile failed");
            }
            out.println();
        }

        // Also: who calls FUN_00ab7740 (the outer trigger)? That tells us
        // WHEN this queue gets drained - once per frame, or something else.
        out.println("=== callers of FUN_00ab7740 ===");
        Address addr = currentProgram.getAddressFactory().getAddress("00ab7740");
        ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(addr);
        Set<String> seen = new HashSet<>();
        while (refs.hasNext()) {
            Reference r = refs.next();
            Function callerFunc = getFunctionContaining(r.getFromAddress());
            String key = callerFunc != null ? callerFunc.getEntryPoint().toString() : r.getFromAddress().toString();
            if (seen.contains(key)) continue;
            seen.add(key);
            out.printf("  from %s (in %s)%n", r.getFromAddress(),
                    callerFunc != null ? callerFunc.getName() + "@" + callerFunc.getEntryPoint() : "?");
        }

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
