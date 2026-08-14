import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// CinemaController singleton = DAT_050f5a90 (built at boot by FUN_00910ac0,
// called from FUN_00432200; it is a live, fully-implemented subsystem - its
// vtable slots are distinct real functions, unlike the gutted debug menu).
//
// Now find a field that says "a cutscene is currently playing", so the mod can
// force ShadowSplitNear back to default during cinema and restore afterwards.
// Strategy: dump every function that touches the singleton; the SMALL ones are
// accessors and one of them should be the is-playing / get-state query.
public class CinemaAccessors extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/cinema_accessors.txt", "UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        Address g = toAddr(0x50f5a90L);
        Map<Address, Long> sizes = new LinkedHashMap<>();
        out.println("=== all references to CinemaController singleton DAT_050f5a90 ===");
        for (Reference r : getReferencesTo(g)) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.println("  " + r.getFromAddress() + " " + r.getReferenceType()
                + (f != null ? "  in " + f.getName() + "@" + f.getEntryPoint()
                               + " size=" + f.getBody().getNumAddresses() : "  (data)"));
            if (f != null) sizes.put(f.getEntryPoint(), f.getBody().getNumAddresses());
        }
        out.println("=== " + sizes.size() + " distinct functions ===");

        // decompile smallest first - accessors
        List<Map.Entry<Address, Long>> es = new ArrayList<>(sizes.entrySet());
        es.sort(Comparator.comparingLong(Map.Entry::getValue));
        int n = 0;
        for (Map.Entry<Address, Long> e : es) {
            if (e.getValue() > 400 || n++ > 25) continue;
            Function f = getFunctionAt(e.getKey());
            out.println("############################################################");
            out.println("### " + f.getName() + " @ " + e.getKey() + "  size=" + e.getValue());
            out.print("### callers:");
            int c = 0;
            for (Reference r : getReferencesTo(e.getKey())) {
                Function cf = getFunctionContaining(r.getFromAddress());
                out.print(cf != null ? " " + cf.getName() : " (data@" + r.getFromAddress() + ")");
                if (++c > 8) { out.print(" ..."); break; }
            }
            out.println();
            DecompileResults res = dec.decompileFunction(f, 90, new ConsoleTaskMonitor());
            if (res != null && res.decompileCompleted())
                out.println(res.getDecompiledFunction().getC());
        }
        out.close();
        println("done");
    }
}
