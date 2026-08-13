import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// The cascade split value is uploaded to shaders as "shadowSplitRange"
// (string @ 02165e60). FUN_00c5c050 only LOOKS UP its constant handle by
// name; the value itself is written elsewhere. Strategy: find the field
// offset that FUN_00c5c050 stores the handle into, then find who writes the
// matching value field. Simpler first pass: decompile the renderer functions
// that reference the shadow shader-constant name block and the near/far
// shadow matrix names, since whoever fills those also computes the splits.
public class FindSplitRange extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/split_range.txt","UTF-8");
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);

        // shadowSplitRange, shadowFadeParam, s_viewShadowMatrixNearColumn0,
        // shadowMatrix0Column0, screenShadowMap, shadowMap
        long[] strs = { 0x02165e60L, 0x02165e8cL, 0x0215e54cL, 0x02165e48L,
                        0x0217e70cL, 0x0217e75cL };
        Set<String> funcs = new LinkedHashSet<>();
        for (long s : strs) {
            Address a = currentProgram.getAddressFactory().getAddress(Long.toHexString(s));
            Data dt = getDataAt(a);
            out.println("---- refs to " + a + " ("
                        + (dt!=null&&dt.hasStringValue()? dt.getValue().toString():"?") + ") ----");
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
            while (it.hasNext()) {
                Function f = getFunctionContaining(it.next().getFromAddress());
                if (f == null) continue;
                String k = f.getEntryPoint().toString();
                if (funcs.add(k))
                    out.println("   " + f.getName() + " @ " + k
                                + " size=" + f.getBody().getNumAddresses());
            }
        }
        out.println();
        out.flush();

        for (String k : funcs) {
            Function f = getFunctionContaining(currentProgram.getAddressFactory().getAddress(k));
            if (f == null || f.getBody().getNumAddresses() > 6000) {
                out.println("=== " + k + " skipped (too large) ===\n"); continue;
            }
            out.println("=== " + f.getName() + " @ " + k
                        + " size=" + f.getBody().getNumAddresses() + " ===");
            DecompileResults r = d.decompileFunction(f, 90, new ConsoleTaskMonitor());
            out.println(r!=null&&r.decompileCompleted()? r.getDecompiledFunction().getC():"  failed");
            out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
