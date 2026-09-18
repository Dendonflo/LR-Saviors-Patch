import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.*;
import java.util.*;

// Generic: decompile every function whose entry (or containing address) is listed
// in ghidra_output/_decomp_request.txt (one hex address per line), write to
// ghidra_output/_decomp_result.txt. Reused for crash triage.
public class DecompileList extends GhidraScript {
    @Override
    public void run() throws Exception {
        String dir = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/";
        List<String> targets = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(dir + "_decomp_request.txt"))) {
            String s; while ((s = r.readLine()) != null) { s = s.trim(); if (!s.isEmpty()) targets.add(s); }
        }
        PrintWriter out = new PrintWriter(dir + "_decomp_result.txt", "UTF-8");
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();
        for (String a : targets) {
            Address addr = currentProgram.getAddressFactory().getAddress(a);
            Function func = getFunctionContaining(addr);
            if (func == null) { out.printf("=== %s : NO FUNCTION ===%n%n", a); continue; }
            out.printf("=== %s : %s @ %s (size=%d) ===%n", a, func.getName(), func.getEntryPoint(), func.getBody().getNumAddresses());
            out.println("--- Callers ---");
            Map<String, Boolean> seen = new LinkedHashMap<>();
            int cnt = 0;
            for (Reference ref : getReferencesTo(func.getEntryPoint())) {
                Function cf = getFunctionContaining(ref.getFromAddress());
                String key = cf != null ? cf.getName() + " @ " + cf.getEntryPoint() : "addr " + ref.getFromAddress();
                if (!seen.containsKey(key)) { seen.put(key, true); out.println("    " + key); if (++cnt >= 25) { out.println("    ..."); break; } }
            }
            out.println("--- Decompiled ---");
            DecompileResults res = decomp.decompileFunction(func, 120, monitor);
            if (res != null && res.decompileCompleted()) out.println(res.getDecompiledFunction().getC());
            else out.println("DECOMPILE FAILED: " + (res != null ? res.getErrorMessage() : "null"));
            if (res == null || !res.decompileCompleted() || func.getBody().getNumAddresses() < 300) {
                out.println("--- Listing ---");
                Instruction q = currentProgram.getListing().getInstructionAt(func.getEntryPoint());
                int k = 0;
                while (q != null && func.getBody().contains(q.getAddress()) && k++ < 400) {
                    out.printf("    %s  %s%n", q.getAddress(), q.toString());
                    q = q.getNext();
                }
            }
            out.println();
        }
        decomp.dispose(); out.close();
        println("DONE");
    }
}
