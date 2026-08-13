import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.data.StringDataType;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// The frame graph names its main 3D stage MULTI_SAMPLE, with SCHEDULE and
// PROPAGATION sub-passes, and there is an INTERVAL_SHADOW string. Hypothesis:
// the engine runs an interleaved/temporal multi-sampling scheme - a 3x3 grid
// would be 9 samples, one per frame, which is exactly the unexplained 9-frame
// period in the frametime data.
//
// Wanted:
//   - every INTERVAL_* string (what else is on an interval, and how many)
//   - the MULTI_SAMPLE stage handlers, decompiled, looking for a sample count,
//     a modulo, or a rotating index
public class MultiSample extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/multisample.txt","UTF-8");
        Listing lst = currentProgram.getListing();

        out.println("################ INTERVAL_* / SAMPLE-ish strings ################");
        DataIterator di = lst.getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            if (!(d.getDataType() instanceof StringDataType)) continue;
            Object v = d.getValue();
            if (v == null) continue;
            String s = v.toString();
            String u = s.toUpperCase();
            if (u.contains("INTERVAL") || u.contains("MULTI_SAMPLE")
                || u.contains("MULTISAMPLE") || u.contains("JITTER")
                || u.contains("SUBSAMPLE") || u.contains("SUB_SAMPLE")) {
                out.printf("  %-12s \"%s\"%n", d.getAddress(), s);
            }
        }

        out.println();
        out.println("################ MULTI_SAMPLE stage handlers ################");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        // FUN_00b01da0 = DRAW_MULTI_SAMPLE_BEGIN/END handler (from the string refs);
        // FUN_00ac72d0 registers most of the pass names.
        String[] fns = { "00b01da0", "00ac72d0" };
        for (String s : fns) {
            Address a = currentProgram.getAddressFactory().getAddress(s);
            Function f = getFunctionContaining(a);
            out.println("======== FUN_" + s + " ========");
            if (f == null) { out.println("  no function"); continue; }
            out.println("  size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 120, new ConsoleTaskMonitor());
            String c = (r != null && r.getDecompiledFunction() != null)
                       ? r.getDecompiledFunction().getC() : "(failed)";
            // FUN_00ac72d0 is a big registration table; cap it.
            if (c.length() > 9000) c = c.substring(0, 9000) + "\n... [truncated]";
            out.println(c);
        }
        out.close();
        println("wrote multisample.txt");
    }
}
