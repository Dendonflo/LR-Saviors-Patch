// Looking for the enqueue ("push") side of the job queue whose pop side we
// already found (FUN_00a015b0, called from FUN_00a01a00 and FUN_00a01bb0).
// Strategy: list all functions in the same address neighborhood (the job
// system appears to live around 0xa01000-0xa02000 based on FUN_00a01670/
// FUN_00a015b0/FUN_00a01a00/FUN_00a01bb0), decompile the bounds-check helper
// FUN_00a01670 referenced inside the dispatcher, and dump the values of the
// DAT_02... globals used as job-range arguments so we can tell whether
// they're simple integer bounds or pointers to shared state.

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.Map;

public class FindJobEnqueue extends GhidraScript {

    static final String[] DAT_GLOBALS = {
        "0210aa60", "0210ab78", "0210ab80", "0210ad28", "0210ad30", "0210ad90"
    };

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/job_enqueue_search.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        out.println("=== Functions in address range 0x00a00000 - 0x00a02500 (job-system neighborhood) ===");
        Address start = currentProgram.getAddressFactory().getAddress("00a00000");
        Address end = currentProgram.getAddressFactory().getAddress("00a02500");
        FunctionIterator fi = currentProgram.getFunctionManager().getFunctions(new AddressSet(start, end), true);
        for (Function f : fi) {
            out.printf("%s @ %s (size=%d)%n", f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses());
        }

        out.println();
        out.println("=== Values of DAT_02... globals used as FUN_00ab9020 args ===");
        for (String g : DAT_GLOBALS) {
            Address addr = currentProgram.getAddressFactory().getAddress(g);
            try {
                int val = currentProgram.getMemory().getInt(addr);
                out.printf("DAT_%s = 0x%x (%d)%n", g, val, val);
            } catch (Exception e) {
                out.printf("DAT_%s : could not read (%s)%n", g, e.getMessage());
            }
        }

        out.println();
        out.println("=== FUN_00a01670 (bounds-check helper referenced in FUN_00a01a00) ===");
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        Address a1670 = currentProgram.getAddressFactory().getAddress("00a01670");
        Function f1670 = getFunctionContaining(a1670);
        if (f1670 != null) {
            out.println("--- Callers ---");
            Reference[] refs = getReferencesTo(f1670.getEntryPoint());
            Map<String, Boolean> seen = new LinkedHashMap<>();
            for (Reference ref : refs) {
                Function callerFunc = getFunctionContaining(ref.getFromAddress());
                String key = callerFunc != null ? callerFunc.getName() + " @ " + callerFunc.getEntryPoint() : "addr " + ref.getFromAddress();
                if (!seen.containsKey(key)) { seen.put(key, true); out.println("    " + key); }
            }
            out.println("--- Decompiled ---");
            DecompileResults res = decomp.decompileFunction(f1670, 60, monitor);
            out.println(res != null && res.decompileCompleted() ? res.getDecompiledFunction().getC() : "DECOMPILE FAILED");
        } else {
            out.println("NOT FOUND");
        }

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
