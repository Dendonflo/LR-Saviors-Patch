import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;

public class FindClassLoaderCallers extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/classloader_callers.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        String[] targets = {"009a2970", "009a2900", "009a2790"};
        for (String t : targets) {
            Address addr = currentProgram.getAddressFactory().getAddress(t);
            Function func = getFunctionAt(addr);
            out.println("=== callers of FUN_" + t + " ===");
            if (func == null) { out.println("  function not found"); continue; }
            ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(addr);
            int count = 0;
            while (refs.hasNext() && count < 15) {
                Reference r = refs.next();
                Function callerFunc = getFunctionContaining(r.getFromAddress());
                out.printf("  from %s (in %s)%n", r.getFromAddress(),
                        callerFunc != null ? callerFunc.getName() + "@" + callerFunc.getEntryPoint() : "?");
                count++;
            }
            out.println();
        }

        // Decompile the direct caller(s) of FUN_009a2970 to see calling context/frequency
        out.println("=== decompiling callers of FUN_009a2970 ===");
        Address target = currentProgram.getAddressFactory().getAddress("009a2970");
        ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(target);
        java.util.Set<String> done = new java.util.HashSet<>();
        while (refs.hasNext()) {
            Reference r = refs.next();
            Function callerFunc = getFunctionContaining(r.getFromAddress());
            if (callerFunc == null) continue;
            String key = callerFunc.getEntryPoint().toString();
            if (done.contains(key)) continue;
            done.add(key);
            out.println("--- caller function " + callerFunc.getName() + " @ " + callerFunc.getEntryPoint() + " ---");
            DecompileResults res = decomp.decompileFunction(callerFunc, 60, new ConsoleTaskMonitor());
            if (res != null && res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("  decompile failed");
            }
            out.println();
        }

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
