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

public class FindEntityLoopCallers extends GhidraScript {
    private DecompInterface decomp;
    private PrintWriter out;

    private void printCallers(String targetAddr, int depth) throws Exception {
        Address addr = currentProgram.getAddressFactory().getAddress(targetAddr);
        Function func = getFunctionAt(addr);
        out.println("=== callers of FUN_" + targetAddr + (func != null ? " (" + func.getName() + ")" : "") + " ===");
        if (func == null) { out.println("  function not found at this address"); return; }
        ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(addr);
        Set<String> seen = new HashSet<>();
        int count = 0;
        while (refs.hasNext() && count < 20) {
            Reference r = refs.next();
            Function callerFunc = getFunctionContaining(r.getFromAddress());
            String key = callerFunc != null ? callerFunc.getEntryPoint().toString() : r.getFromAddress().toString();
            if (seen.contains(key)) continue;
            seen.add(key);
            out.printf("  from %s (in %s)%n", r.getFromAddress(),
                    callerFunc != null ? callerFunc.getName() + "@" + callerFunc.getEntryPoint() : "?");
            count++;
        }
        out.println();
    }

    private void decompileFunc(String addrStr) throws Exception {
        Address addr = currentProgram.getAddressFactory().getAddress(addrStr);
        Function func = getFunctionAt(addr);
        out.println("--- decompile FUN_" + addrStr + " ---");
        if (func == null) { out.println("  not found"); return; }
        DecompileResults res = decomp.decompileFunction(func, 60, new ConsoleTaskMonitor());
        if (res != null && res.decompileCompleted()) {
            out.println(res.getDecompiledFunction().getC());
        } else {
            out.println("  decompile failed");
        }
        out.println();
    }

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/entity_loop_callers.txt";
        out = new PrintWriter(outPath, "UTF-8");
        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        // Who calls the per-entity-array update loop (FUN_00a41570)?
        printCallers("00a41570", 1);
        // And who calls ITS caller(s), to understand real trigger frequency
        printCallers("00a46c20", 1);

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
