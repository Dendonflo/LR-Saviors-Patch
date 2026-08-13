import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.RefType;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.Map;

public class FindThreadCountConfig extends GhidraScript {

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/thread_count_config.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        out.println("=== FUN_00a01fa0 (returns value compared against DAT_02350664) ===");
        Address a = currentProgram.getAddressFactory().getAddress("00a01fa0");
        Function f = getFunctionContaining(a);
        if (f != null) {
            DecompileResults res = decomp.decompileFunction(f, 60, monitor);
            out.println(res != null && res.decompileCompleted() ? res.getDecompiledFunction().getC() : "DECOMPILE FAILED");
        } else {
            out.println("NOT FOUND");
        }

        out.println();
        out.println("=== All references to DAT_02350664 (candidate worker-count cap) ===");
        Address datAddr = currentProgram.getAddressFactory().getAddress("02350664");
        Reference[] refs = getReferencesTo(datAddr);
        Map<String, Boolean> seen = new LinkedHashMap<>();
        for (Reference ref : refs) {
            Address from = ref.getFromAddress();
            Function func = getFunctionContaining(from);
            RefType rt = ref.getReferenceType();
            String key = (func != null ? func.getName() + " @ " + func.getEntryPoint() : "addr " + from) + " (refType=" + rt + ")";
            if (!seen.containsKey(key)) { seen.put(key, true); out.println("    " + key); }
        }

        out.println();
        out.println("=== Also check FUN_00428140 (thread-index/ID lookup used throughout) ===");
        Address a2 = currentProgram.getAddressFactory().getAddress("00428140");
        Function f2 = getFunctionContaining(a2);
        if (f2 != null) {
            DecompileResults res2 = decomp.decompileFunction(f2, 60, monitor);
            out.println(res2 != null && res2.decompileCompleted() ? res2.getDecompiledFunction().getC() : "DECOMPILE FAILED");
        } else {
            out.println("NOT FOUND");
        }

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
