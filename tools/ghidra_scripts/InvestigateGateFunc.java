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

// FUN_00acbe70 is a boolean-returning gate function called right before
// FUN_00aacf10 in an undefined code fragment at 0x00acc120 (found sitting
// between two recognized functions, never given its own Function boundary
// by Ghidra). This is the leading candidate for "the actual per-frame
// relevance/activation check" that decides whether a character's skeleton
// gets processed that frame. Investigates: who calls the 0x00acc120
// fragment, and fully decompiles FUN_00acbe70 plus its own callers/callees.
public class InvestigateGateFunc extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/gate_func_investigation.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        out.println("=== Who calls the undefined fragment at 0x00acc120? ===");
        Address fragAddr = currentProgram.getAddressFactory().getAddress("00acc120");
        Reference[] fragRefs = getReferencesTo(fragAddr);
        out.printf("Found %d references%n", fragRefs.length);
        for (Reference r : fragRefs) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.println("    from " + r.getFromAddress() + " (" + (f != null ? f.getName() + " @ " + f.getEntryPoint() : "no func") + ") type=" + r.getReferenceType());
        }

        out.println();
        out.println("=== Attempting to create/find a Function at 0x00acc120 ===");
        Function existing = getFunctionAt(fragAddr);
        if (existing == null) {
            Function created = createFunction(fragAddr, "FRAG_00acc120");
            out.println("createFunction result: " + (created != null ? "OK: " + created.getName() : "FAILED"));
        } else {
            out.println("already exists: " + existing.getName());
        }

        String[] targets = {"00acbe70"};
        for (String addrStr : targets) {
            Address addr = currentProgram.getAddressFactory().getAddress(addrStr);
            Function func = getFunctionContaining(addr);
            out.println();
            if (func == null) { out.printf("=== %s NO FUNCTION ===%n", addrStr); continue; }
            out.printf("=== %s @ %s (size=%d) ===%n", func.getName(), func.getEntryPoint(), func.getBody().getNumAddresses());
            out.println("--- Callers ---");
            Reference[] refs = getReferencesTo(func.getEntryPoint());
            Map<String, Boolean> seen = new LinkedHashMap<>();
            int cnt = 0;
            for (Reference ref : refs) {
                Function callerFunc = getFunctionContaining(ref.getFromAddress());
                String key = callerFunc != null ? callerFunc.getName() + " @ " + callerFunc.getEntryPoint() : "addr " + ref.getFromAddress();
                if (!seen.containsKey(key)) { seen.put(key, true); out.println("    " + key); if (++cnt>=20) break; }
            }
            out.println("--- Decompiled ---");
            DecompileResults res = decomp.decompileFunction(func, 60, monitor);
            out.println(res != null && res.decompileCompleted() ? res.getDecompiledFunction().getC() : "DECOMPILE FAILED");
        }

        // Now try decompiling the newly created fragment function too
        out.println();
        Function fragFunc = getFunctionAt(fragAddr);
        if (fragFunc != null) {
            out.printf("=== %s @ %s (size=%d) ===%n", fragFunc.getName(), fragFunc.getEntryPoint(), fragFunc.getBody().getNumAddresses());
            out.println("--- Callers ---");
            Reference[] refs = getReferencesTo(fragFunc.getEntryPoint());
            for (Reference ref : refs) {
                Function callerFunc = getFunctionContaining(ref.getFromAddress());
                out.println("    " + (callerFunc != null ? callerFunc.getName() + " @ " + callerFunc.getEntryPoint() : "addr " + ref.getFromAddress()));
            }
            out.println("--- Decompiled ---");
            DecompileResults res = decomp.decompileFunction(fragFunc, 60, monitor);
            out.println(res != null && res.decompileCompleted() ? res.getDecompiledFunction().getC() : "DECOMPILE FAILED");
        }

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
