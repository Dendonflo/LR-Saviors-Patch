import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.Map;

public class DecompilePacerHelpers extends GhidraScript {
    static final String[] TARGETS = {"00a96110", "00a96590", "00a96f20", "00a933a0", "00a96090"};

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/pacer_helpers_decompiled.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        for (String addrStr : TARGETS) {
            Address addr = currentProgram.getAddressFactory().getAddress(addrStr);
            Function func = getFunctionContaining(addr);
            if (func == null) { out.printf("=== %s NO FUNCTION ===%n%n", addrStr); continue; }
            out.printf("=== %s @ %s (size=%d) ===%n", func.getName(), func.getEntryPoint(), func.getBody().getNumAddresses());
            out.println("--- Callers ---");
            Reference[] refs = getReferencesTo(func.getEntryPoint());
            Map<String, Boolean> seen = new LinkedHashMap<>();
            int cnt = 0;
            for (Reference ref : refs) {
                Function callerFunc = getFunctionContaining(ref.getFromAddress());
                String key = callerFunc != null ? callerFunc.getName() + " @ " + callerFunc.getEntryPoint() : "addr " + ref.getFromAddress();
                if (!seen.containsKey(key)) { seen.put(key, true); out.println("    " + key); if (++cnt>=10) break; }
            }
            out.println("--- Decompiled ---");
            DecompileResults res = decomp.decompileFunction(func, 60, monitor);
            out.println(res != null && res.decompileCompleted() ? res.getDecompiledFunction().getC() : "DECOMPILE FAILED");
            out.println();
        }
        decomp.dispose();
        out.close();
        println("DONE");
    }
}
