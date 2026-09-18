import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Yusnaan flanitor crash (2026-09-18): EIP 0073CF03 reading [reg+0x348] with reg=0.
// Decompile the faulting function + every exe address the raw stack sweep printed.
public class DecompileCrashYusnaan extends GhidraScript {
    static final String[] TARGETS = {
        "0073cf03", "005eb764", "0073cfbe", "005efeb5", "009fbcd7", "00773031",
        "00733030", "00694867", "005f0000", "00a46572", "005c5027", "005cc551",
        "005e49a7", "00600282"
    };
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/crash_yusnaan_flanitor.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();
        Set<String> done = new HashSet<>();
        for (String a : TARGETS) {
            Address addr = currentProgram.getAddressFactory().getAddress(a);
            Function func = getFunctionContaining(addr);
            if (func == null) { out.printf("=== %s : NO FUNCTION ===%n%n", a); continue; }
            out.printf("=== %s : in %s @ %s (size=%d) ===%n", a, func.getName(), func.getEntryPoint(), func.getBody().getNumAddresses());
            // listing around the address
            Listing lst = currentProgram.getListing();
            Instruction ins = lst.getInstructionContaining(addr);
            if (ins != null) {
                Instruction p = ins;
                for (int i = 0; i < 12 && p != null; i++) p = p.getPrevious();
                if (p == null) p = lst.getInstructionAt(func.getEntryPoint());
                Instruction q = p;
                for (int i = 0; i < 24 && q != null; i++) {
                    out.printf("    %s%s  %s%n", q.getAddress().equals(ins.getAddress()) ? ">>" : "  ", q.getAddress(), q.toString());
                    q = q.getNext();
                }
            }
            if (done.contains(func.getEntryPoint().toString())) { out.println("    (already decompiled above)\n"); continue; }
            done.add(func.getEntryPoint().toString());
            out.println("--- Callers ---");
            Map<String, Boolean> seen = new LinkedHashMap<>();
            int cnt = 0;
            for (Reference ref : getReferencesTo(func.getEntryPoint())) {
                Function cf = getFunctionContaining(ref.getFromAddress());
                String key = cf != null ? cf.getName() + " @ " + cf.getEntryPoint() : "addr " + ref.getFromAddress();
                if (!seen.containsKey(key)) { seen.put(key, true); out.println("    " + key); if (++cnt >= 20) { out.println("    ..."); break; } }
            }
            out.println("--- Decompiled ---");
            DecompileResults res = decomp.decompileFunction(func, 90, monitor);
            if (res != null && res.decompileCompleted()) out.println(res.getDecompiledFunction().getC());
            else out.println("DECOMPILE FAILED: " + (res != null ? res.getErrorMessage() : "null"));
            out.println();
        }
        decomp.dispose(); out.close();
        println("DONE " + outPath);
    }
}
