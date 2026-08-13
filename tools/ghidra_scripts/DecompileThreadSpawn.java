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

public class DecompileThreadSpawn extends GhidraScript {

    static final String[][] TARGETS = {
        {"00725790", "named_thread_spawn_util"},
        {"004b9070", "asyncloader_enqueue_caller_1"}
    };

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/tools/ghidra_output/thread_spawn_decompiled.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        for (String[] t : TARGETS) {
            String addrStr = t[0];
            String label = t[1];
            Address addr = currentProgram.getAddressFactory().getAddress(addrStr);
            Function func = getFunctionContaining(addr);
            if (func == null) {
                out.printf("=== %s (%s) : NO FUNCTION FOUND ===%n%n", addrStr, label);
                continue;
            }

            out.printf("=== %s : %s @ %s ===%n", label, func.getName(), func.getEntryPoint());

            out.println("--- Callers ---");
            Reference[] refs = getReferencesTo(func.getEntryPoint());
            Map<String, Boolean> seen = new LinkedHashMap<>();
            for (Reference ref : refs) {
                Address fromAddr = ref.getFromAddress();
                Function callerFunc = getFunctionContaining(fromAddr);
                String key = callerFunc != null ? callerFunc.getName() + " @ " + callerFunc.getEntryPoint() : "addr " + fromAddr;
                if (!seen.containsKey(key)) {
                    seen.put(key, true);
                    out.println("    " + key + " (refType=" + ref.getReferenceType() + ")");
                }
            }

            out.println("--- Decompiled ---");
            DecompileResults res = decomp.decompileFunction(func, 60, monitor);
            if (res != null && res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("DECOMPILE FAILED: " + (res != null ? res.getErrorMessage() : "null result"));
            }
            out.println();
        }

        decomp.dispose();
        out.close();
        println("DONE. Written to " + outPath);
    }
}
