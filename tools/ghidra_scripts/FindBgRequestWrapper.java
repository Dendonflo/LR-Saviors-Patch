// FUN_004b5cb0's dispatch switch has case 0x3e calling BgLoader::prefetch_map
// (FUN_00494f40). Decompile the highest-address enqueue-side wrappers (callers
// of FUN_004b3600) to find which one tags its request struct with type=0x3e,
// and check whether that wrapper (or its own caller) waits for completion.

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

public class FindBgRequestWrapper extends GhidraScript {

    static final String[] TARGETS = {
        "004b5bf0", "004b59d0", "004b58c0", "004b5830", "004b57a0",
        "004b5670", "004b55e0", "004b5550", "004b54c0", "004b5430"
    };

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/tools/ghidra_output/bg_request_wrappers.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        for (String addrStr : TARGETS) {
            Address addr = currentProgram.getAddressFactory().getAddress(addrStr);
            Function func = getFunctionContaining(addr);
            if (func == null) {
                out.printf("=== %s : NO FUNCTION FOUND ===%n%n", addrStr);
                continue;
            }

            out.printf("=== %s @ %s ===%n", func.getName(), func.getEntryPoint());
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
                out.println("DECOMPILE FAILED");
            }
            out.println();
        }

        decomp.dispose();
        out.close();
        println("DONE. Written to " + outPath);
    }
}
