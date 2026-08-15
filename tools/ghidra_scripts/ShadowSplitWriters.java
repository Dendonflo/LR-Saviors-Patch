import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.*;

// Three shadow questions, 2026-08-15:
//  (3) does shadow map RESOLUTION also move the cascade split distances?
//      Resolution lives in the settings object (DAT_0511558c) at +0x24;
//      the splits live in the scene object (DAT_05107a00) at +0x42c (near)
//      and +0x430 * +0x438 (far). Different objects - but the engine could
//      still derive one from the other. Find every function that touches
//      either and look for overlap.
//  (4) the user reports OTHER, longer-range high-quality shadows that
//      ignore the near split but DO respond to resolution. A second
//      projection reading a different distance field would explain it, so
//      dump every float field read near the split fields too.
//
// Strategy: scan the whole text section for instructions with a constant
// displacement matching the fields of interest, group by containing
// function, and decompile the ones that touch more than one.
public class ShadowSplitWriters extends GhidraScript {

    static final int OFF_NEAR = 0x42c, OFF_FARA = 0x430, OFF_FARB = 0x438;
    static final int OFF_RES  = 0x24;

    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/shadow_split_writers.txt", "UTF-8");

        Map<String, Set<Integer>> hits = new TreeMap<>();
        Map<String, Function> fns = new HashMap<>();
        int[] wanted = { OFF_NEAR, OFF_FARA, OFF_FARB };

        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            for (int op = 0; op < ins.getNumOperands(); op++) {
                for (Object o : ins.getOpObjects(op)) {
                    if (!(o instanceof Scalar)) continue;
                    int v = (int) ((Scalar) o).getUnsignedValue();
                    for (int w : wanted) {
                        if (v != w) continue;
                        Function f = getFunctionContaining(ins.getAddress());
                        if (f == null) continue;
                        String k = f.getName() + " @ " + f.getEntryPoint();
                        hits.computeIfAbsent(k, x -> new TreeSet<>()).add(w);
                        fns.put(k, f);
                    }
                }
            }
        }

        out.println("=== functions referencing cascade split offsets ===");
        out.println("(0x42c = near, 0x430 = far factor A, 0x438 = far factor B)\n");
        for (Map.Entry<String, Set<Integer>> e : hits.entrySet()) {
            StringBuilder sb = new StringBuilder();
            for (int v : e.getValue()) sb.append(String.format("0x%x ", v));
            out.println("  " + e.getKey() + "   touches: " + sb);
        }
        out.println();

        // Decompile the ones touching two or more of the split fields - those
        // are the cascade setup sites, where any resolution dependency would
        // have to appear.
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        for (Map.Entry<String, Set<Integer>> e : hits.entrySet()) {
            if (e.getValue().size() < 2) continue;
            Function f = fns.get(e.getKey());
            out.println("################ " + e.getKey() + " ################");
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                Function c = getFunctionContaining(r.getFromAddress());
                out.println("// caller: " + r.getFromAddress() + " in "
                            + (c != null ? c.getName() : "?"));
            }
            DecompileResults res = dec.decompileFunction(f, 120, monitor);
            out.println(res != null && res.decompileCompleted()
                        ? res.getDecompiledFunction().getC() : "  DECOMPILE FAILED");
            out.println();
        }
        dec.dispose();
        out.close();
        println("done -> shadow_split_writers.txt");
    }
}
