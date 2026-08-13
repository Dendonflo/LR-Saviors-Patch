import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Backwards from the VALUE. FUN_00c5c050 binds constant handles into a struct
// at *(param_1+0x90). Whoever UPLOADS shadowSplitRange reads that struct and
// has the computed split in hand - and the same code computes the cascade
// extents.
//
// FUN_00c5c050's caller owns the object, so start there and walk its class.
public class SplitProducer extends GhidraScript {
    PrintWriter out; DecompInterface d; Set<String> done = new LinkedHashSet<>();

    void dump(Function f, String why) {
        if (f == null) return;
        String k = f.getEntryPoint().toString();
        if (!done.add(k)) return;
        long sz = f.getBody().getNumAddresses();
        out.println("=== " + f.getName() + " @ " + k + " size=" + sz + " [" + why + "] ===");
        if (sz > 9000) { out.println("  (too large)\n"); return; }
        DecompileResults r = d.decompileFunction(f, 150, new ConsoleTaskMonitor());
        out.println(r != null && r.decompileCompleted() ? r.getDecompiledFunction().getC() : " failed");
        out.println(); out.flush();
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/split_producer.txt","UTF-8");
        d = new DecompInterface(); d.openProgram(currentProgram);

        // who calls the handle-binder?
        out.println("######## callers of FUN_00c5c050 (handle binder) ########");
        Address a = currentProgram.getAddressFactory().getAddress("00c5c050");
        List<Function> callers = new ArrayList<>();
        for (Reference r : getReferencesTo(a)) {
            Function f = getFunctionContaining(r.getFromAddress());
            if (f != null) { callers.add(f); out.println("   " + f.getName() + " @ " + f.getEntryPoint()
                             + " size=" + f.getBody().getNumAddresses()); }
        }
        out.println();
        for (Function f : callers) dump(f, "caller of handle binder");

        // and their callers - the per-frame updater should be up there
        Set<Function> up = new LinkedHashSet<>();
        for (Function f : callers)
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                Function g = getFunctionContaining(r.getFromAddress());
                if (g != null) up.add(g);
            }
        for (Function f : up) dump(f, "caller-of-caller");

        d.dispose(); out.close(); println("DONE");
    }
}
