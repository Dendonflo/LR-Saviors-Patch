import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;
import java.util.*;

// Hunting the shadow-map setup code.
//
// Runtime facts established by the RT inventory probe (see FEATURES.md):
//   - shadow set is 2048x4096 R32F atlas + 2x 2048x2048 R32F + 2x 2048x2048
//     D24S8 at 4K output; half those sizes at 720p, so size is derived, not
//     fixed;
//   - scaling the TEXTURES alone confines shadows to a camera-tracking
//     square, and the engine never calls SetViewport for that pass -
//     so the shadow projection is almost certainly computed from a
//     size the engine holds itself, not queried from the surface.
//
// This script looks for that code by its own naming: the engine tags
// subsystems with dotted strings ("CDev.Dw.Shader.Constant",
// "CDev.Vfx.Qix.QixModel"), so shadow code should be similarly labelled.
public class FindShadowSetup extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/shadow_setup.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        Listing listing = currentProgram.getListing();

        // 1) every string mentioning shadow, in any casing
        out.println("======== strings containing 'shadow' ========");
        List<Data> hits = new ArrayList<>();
        for (Data d : listing.getDefinedData(true)) {
            if (!d.hasStringValue()) continue;
            Object v = d.getValue();
            if (v == null) continue;
            String s = v.toString();
            if (s.toLowerCase().contains("shadow")) {
                hits.add(d);
                out.println("  " + d.getAddress() + "  \"" + s + "\"");
            }
        }
        out.println("  (" + hits.size() + " strings)\n");

        // 2) which functions reference them
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        Set<String> done = new LinkedHashSet<>();
        out.println("======== functions referencing those strings ========");
        for (Data d : hits) {
            ReferenceIterator refs = currentProgram.getReferenceManager()
                    .getReferencesTo(d.getAddress());
            while (refs.hasNext()) {
                Reference r = refs.next();
                Function f = getFunctionContaining(r.getFromAddress());
                if (f == null) continue;
                String key = f.getEntryPoint().toString();
                if (!done.add(key)) continue;
                out.println("  " + f.getName() + " @ " + key
                            + "  size=" + f.getBody().getNumAddresses()
                            + "  (via " + d.getAddress() + ")");
            }
        }
        out.println();
        out.flush();

        // 3) decompile them - capped so the file stays readable
        out.println("======== decompiles ========");
        int n = 0;
        for (String key : done) {
            if (++n > 12) { out.println("... (" + (done.size() - 12) + " more not decompiled)"); break; }
            Address a = currentProgram.getAddressFactory().getAddress(key);
            Function f = getFunctionContaining(a);
            if (f == null) continue;
            out.println("=== " + f.getName() + " @ " + key + " ===");
            DecompileResults res = decomp.decompileFunction(f, 90, new ConsoleTaskMonitor());
            out.println(res != null && res.decompileCompleted()
                        ? res.getDecompiledFunction().getC() : "  decompile failed");
            out.println();
            out.flush();
        }

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
