import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Objective: find where the cascade EXTENT (world area per cascade) is
// computed. Known: FUN_00b010c0 allocates the 5 shadow surfaces into
// param_1+0xa0..0xb0; FUN_00b014b0 rebuilds them on resolution change.
// Whoever RENDERS into those surfaces must build the light projection first.
// Walk callers upward and decompile the neighbourhood.
public class CascadeWalk extends GhidraScript {
    PrintWriter out;
    DecompInterface d;
    Set<String> done = new LinkedHashSet<>();

    void dump(Function f, String why) {
        if (f == null) return;
        String k = f.getEntryPoint().toString();
        if (!done.add(k)) return;
        long sz = f.getBody().getNumAddresses();
        out.println("=== " + f.getName() + " @ " + k + " size=" + sz + "   [" + why + "] ===");
        if (sz > 9000) { out.println("  (too large, skipped)\n"); return; }
        DecompileResults r = d.decompileFunction(f, 90, new ConsoleTaskMonitor());
        out.println(r != null && r.decompileCompleted() ? r.getDecompiledFunction().getC() : "  failed");
        out.println();
        out.flush();
    }

    List<Function> callersOf(String addr) {
        List<Function> res = new ArrayList<>();
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        for (Reference r : getReferencesTo(a)) {
            Function f = getFunctionContaining(r.getFromAddress());
            if (f != null) res.add(f);
        }
        return res;
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/cascade_walk.txt","UTF-8");
        d = new DecompInterface(); d.openProgram(currentProgram);

        // level 1: who calls the allocator and the rebuild check
        for (String seed : new String[]{"00b010c0","00b014b0","00b00b90"}) {
            out.println("######## callers of " + seed + " ########");
            for (Function f : callersOf(seed)) out.println("   " + f.getName() + " @ " + f.getEntryPoint());
            out.println();
        }
        out.flush();

        // level 2: decompile those callers
        Set<Function> lvl1 = new LinkedHashSet<>();
        for (String seed : new String[]{"00b010c0","00b014b0"}) lvl1.addAll(callersOf(seed));
        for (Function f : lvl1) dump(f, "caller of allocator/rebuild");

        // level 3: their callers too - the render entry should be up here
        Set<Function> lvl2 = new LinkedHashSet<>();
        for (Function f : lvl1) lvl2.addAll(callersOf(f.getEntryPoint().toString()));
        for (Function f : lvl2) dump(f, "caller-of-caller");

        d.dispose(); out.close(); println("DONE");
    }
}
