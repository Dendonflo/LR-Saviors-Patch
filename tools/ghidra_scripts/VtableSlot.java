import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.*;
import java.util.*;

// For every RTTI vftable symbol whose name matches FILTER, read slot SLOT_OFF
// and report the target; decompile each distinct target once.
public class VtableSlot extends GhidraScript {
    static final String FILTER = "white::scene::";
    static final int SLOT_OFF = 0x140;
    @Override
    public void run() throws Exception {
        String dir = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/";
        PrintWriter out = new PrintWriter(dir + "_vtable_slot.txt", "UTF-8");
        Memory mem = currentProgram.getMemory();
        Map<Address, List<String>> targets = new LinkedHashMap<>();
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        while (it.hasNext()) {
            Symbol s = it.next();
            String n = s.getName(true);
            if (!n.endsWith("vftable") || !n.contains(FILTER)) continue;
            try {
                int v = mem.getInt(s.getAddress().add(SLOT_OFF));
                Address t = toAddr(v & 0xffffffffL);
                Function f = getFunctionContaining(t);
                out.printf("%-60s @%s slot+0x%x -> %s %s%n", n, s.getAddress(), SLOT_OFF, t, f != null ? f.getName() : "?");
                targets.computeIfAbsent(t, k -> new ArrayList<>()).add(n);
            } catch (Exception e) { out.println(n + " : " + e); }
        }
        out.println();
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();
        for (Map.Entry<Address, List<String>> e : targets.entrySet()) {
            Function f = getFunctionContaining(e.getKey());
            out.println("=== " + e.getKey() + " (" + (f != null ? f.getName() : "?") + ") used by " + e.getValue().size() + " vtables ===");
            if (f == null) continue;
            DecompileResults res = decomp.decompileFunction(f, 60, monitor);
            out.println(res != null && res.decompileCompleted() ? res.getDecompiledFunction().getC() : "FAILED");
        }
        decomp.dispose(); out.close(); println("DONE");
    }
}
