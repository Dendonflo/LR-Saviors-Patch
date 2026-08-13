// Decompiles a hardcoded list of functions of interest (identified via
// FindStreamingStrings.java as being the referrers of BgLoader/AsyncLoader/
// PackResource debug strings), plus lists their direct callers and callees,
// so we can read the actual streaming/load-unload logic in one pass.

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

public class DecompileLoaderFuncs extends GhidraScript {

    static final String[][] TARGETS = {
        {"004b3600", "AsyncLoader_QueSizeOver_ctx"},
        {"004b44d0", "AsyncLoader_request_enqueue"},
        {"00494b30", "BgLoader_unload_block"},
        {"00494f40", "BgLoader_prefetch_map"},
        {"00496070", "BgLoader_load_block"},
        {"00496f20", "BgLoader_misc_ref"},
        {"004a2a50", "Phy_unload"},
        {"004a59d0", "Effect_unload"},
        {"004a5a90", "Cut_unload"},
        {"004a5f00", "Action_unload"},
        {"0049ad10", "Scene_unload"},
        {"0049af30", "VLeafInstance_unload"},
        {"004a8c70", "static_pack_unload_a"},
        {"004a8df0", "static_pack_unload_b"},
        {"004ad4e0", "unloadAllOfNonResidentResources"},
        {"004b1040", "unload_request_chrspec"},
        {"0049c9e0", "unload_Navimap"},
        {"0049ca60", "unload_SaveIcon"},
        {"0049ce40", "unload_battle_data"},
        {"004a8a40", "load_SaveIcon"},
        {"00497890", "PackResource_ref"},
        {"004986a0", "Wdb_ref"}
    };

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/tools/ghidra_output/loader_decompiled.txt";
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
                out.printf("=== %s (%s) : NO FUNCTION FOUND AT/CONTAINING THIS ADDRESS ===%n%n", addrStr, label);
                continue;
            }

            out.printf("=== %s : %s @ %s (containing addr %s) ===%n", label, func.getName(), func.getEntryPoint(), addrStr);

            // Callers
            out.println("--- Callers ---");
            Reference[] refs = getReferencesTo(func.getEntryPoint());
            Map<String, Boolean> seen = new LinkedHashMap<>();
            for (Reference ref : refs) {
                Address fromAddr = ref.getFromAddress();
                Function callerFunc = getFunctionContaining(fromAddr);
                String key = callerFunc != null ? callerFunc.getName() + " @ " + callerFunc.getEntryPoint() : "addr " + fromAddr;
                if (!seen.containsKey(key)) {
                    seen.put(key, true);
                    out.println("    " + key);
                }
            }

            // Decompilation
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
