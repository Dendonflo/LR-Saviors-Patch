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

public class DecompileShaderQueueTrigger extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/shader_queue_trigger.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        Address addr = currentProgram.getAddressFactory().getAddress("0042d2c0");
        Function func = getFunctionAt(addr);
        out.println("=== FUN_0042d2c0 (caller of FUN_00ab7740) ===");
        if (func != null) {
            DecompileResults res = decomp.decompileFunction(func, 60, new ConsoleTaskMonitor());
            if (res != null && res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("  decompile failed");
            }
        } else {
            out.println("  not found");
        }
        out.println();

        // Who calls FUN_0042d2c0? Establishes real trigger frequency.
        out.println("=== callers of FUN_0042d2c0 ===");
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
        out.println();

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
