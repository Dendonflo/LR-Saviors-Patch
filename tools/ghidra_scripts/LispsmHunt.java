import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Generic callee walker (depth 2) used for the LiSPSM / pop-scheduler
// hunts of 2026-09-14/15. Set `root` below and the output file name; every
// function is decompiled and tagged [SQRT] [TRIG] [WRITES-MATRIX-SLOTS].
public class LispsmHunt extends GhidraScript {
    private DecompInterface dec;
    private PrintWriter out;
    private Set<String> seen = new HashSet<>();

    private String decomp(Function f, int cap) {
        DecompileResults r = dec.decompileFunction(f, 240, new ConsoleTaskMonitor());
        String c = (r != null && r.getDecompiledFunction() != null)
                   ? r.getDecompiledFunction().getC() : "(decompile failed)";
        if (c.length() > cap) c = c.substring(0, cap) + "\n... [truncated]";
        return c;
    }

    private void walk(Function f, int depth, int maxDepth) {
        if (f == null || !seen.add(f.getName())) return;
        String c = decomp(f, 40000);
        boolean sqrt = c.contains("SQRT") || c.contains("_CIsqrt") || c.contains("sqrtf");
        boolean trig = c.contains("_CIacos") || c.contains("_CIasin") || c.contains("_CIatan") || c.contains("_CIsin") || c.contains("_CIcos");
        boolean mat  = c.contains("DAT_05107a00 + 0x20") || c.contains("DAT_05107a00 + 0xe0");
        boolean size = f.getBody().getNumAddresses() > 60;
        out.println("################ " + f.getName() + " @ " + f.getEntryPoint()
                    + " depth=" + depth + " size=" + f.getBody().getNumAddresses()
                    + (sqrt ? " [SQRT]" : "") + (trig ? " [TRIG]" : "") + (mat ? " [WRITES-MATRIX-SLOTS]" : "")
                    + " ################");
        out.println(c);
        out.println();
        if (depth >= maxDepth || !size) return;
        for (Function cf : f.getCalledFunctions(new ConsoleTaskMonitor())) {
            String n = cf.getName();
            if (n.startsWith("_") || n.contains("security") || n.startsWith("thunk_FUN_004")) continue;
            walk(cf, depth + 1, maxDepth);
        }
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/pop_sched.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Function root = getFunctionAt(toAddr(0x0049a310));
        walk(root, 0, 2);
        // Also: who WRITES the matrix slots at all (xrefs to code touching
        // DAT_05107a00 then +0x20.. is not resolvable by xref; instead list
        // callers of the root so the frame position is known).
        out.println("======== callers of FUN_00a5d600 ========");
        for (Reference r : getReferencesTo(root.getEntryPoint())) {
            Function cf = getFunctionContaining(r.getFromAddress());
            out.println("  " + r.getFromAddress() + " in " + (cf == null ? "?" : cf.getName()));
        }
        out.close();
        println("wrote lispsm_hunt.txt");
    }
}
