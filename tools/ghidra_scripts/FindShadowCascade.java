import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Cascade / shadow-distance hunt.
//
// The resolution was found at [*(0x0511558c) + 0x24] - i.e. 0x0511558c holds
// a pointer to the graphics-settings object, and the debug menu writes
// fields inside it. Other shadow tunables (cascade split distance, fade)
// are very likely NEARBY FIELDS IN THE SAME OBJECT, so every function that
// touches 0x0511558c is a candidate.
//
// Also chasing "?debug_menu_effect_shadow_config" - a dedicated shadow debug
// menu would expose exactly these tunables, and the resolution was found via
// its debug-menu handler, so the same trick should work twice.
public class FindShadowCascade extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/shadow_cascade.txt","UTF-8");
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);

        long[] targets = { 0x0511558cL, 0x020ae733L };
        Set<String> funcs = new LinkedHashSet<>();
        for (long t : targets) {
            Address a = currentProgram.getAddressFactory().getAddress(Long.toHexString(t));
            out.println("======== references to " + a + " ========");
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
            int n = 0;
            while (it.hasNext()) {
                Reference r = it.next();
                Function f = getFunctionContaining(r.getFromAddress());
                if (f == null) continue;
                n++;
                String key = f.getEntryPoint().toString();
                if (funcs.add(key))
                    out.println("  " + f.getName() + " @ " + key
                                + " size=" + f.getBody().getNumAddresses()
                                + " (from " + r.getFromAddress() + ")");
            }
            out.println("  total refs=" + n + "\n");
        }
        out.flush();

        out.println("======== decompiles (small functions first - setters are tiny) ========");
        List<String> ordered = new ArrayList<>(funcs);
        ordered.sort((x,y) -> {
            Function fx = getFunctionContaining(currentProgram.getAddressFactory().getAddress(x));
            Function fy = getFunctionContaining(currentProgram.getAddressFactory().getAddress(y));
            long sx = fx==null?Long.MAX_VALUE:fx.getBody().getNumAddresses();
            long sy = fy==null?Long.MAX_VALUE:fy.getBody().getNumAddresses();
            return Long.compare(sx, sy);
        });
        int cnt = 0;
        for (String key : ordered) {
            if (++cnt > 30) { out.println("... (" + (ordered.size()-30) + " more)"); break; }
            Function f = getFunctionContaining(currentProgram.getAddressFactory().getAddress(key));
            if (f == null) continue;
            out.println("=== " + f.getName() + " @ " + key
                        + " size=" + f.getBody().getNumAddresses() + " ===");
            DecompileResults r = d.decompileFunction(f, 60, new ConsoleTaskMonitor());
            out.println(r!=null&&r.decompileCompleted()? r.getDecompiledFunction().getC():"  failed");
            out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
