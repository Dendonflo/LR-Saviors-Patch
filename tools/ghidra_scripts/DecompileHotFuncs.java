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

public class DecompileHotFuncs extends GhidraScript {

    static final String[][] TARGETS = {
        {"00ac3040", "hottest_106hits"},
        {"0040acb0", "2nd_70hits"},
        {"00416140", "3rd_55hits"},
        {"004164f0", "4th_54hits"},
        {"00a015b0", "5th_49hits_a"},
        {"00b8edb0", "5th_49hits_b"},
        {"00b49790", "7th_35hits"},
        {"00aac130", "8th_31hits"}
    };

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/hot_funcs_decompiled.txt";
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

            out.printf("=== %s : %s @ %s (size=%d bytes) ===%n", label, func.getName(), func.getEntryPoint(), func.getBody().getNumAddresses());

            out.println("--- Callers ---");
            Reference[] refs = getReferencesTo(func.getEntryPoint());
            Map<String, Boolean> seen = new LinkedHashMap<>();
            int cnt = 0;
            for (Reference ref : refs) {
                Address fromAddr = ref.getFromAddress();
                Function callerFunc = getFunctionContaining(fromAddr);
                String key = callerFunc != null ? callerFunc.getName() + " @ " + callerFunc.getEntryPoint() : "addr " + fromAddr;
                if (!seen.containsKey(key)) {
                    seen.put(key, true);
                    out.println("    " + key);
                    if (++cnt >= 15) { out.println("    ..."); break; }
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
