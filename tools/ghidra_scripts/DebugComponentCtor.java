import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Last link of the debug-menu enable chain. The tick (AppGame vslot 0x164 =
// FUN_0042a690) requires AppGame+0x658 != 0 and singleton DAT_024c3d74.
// Find every reference to DAT_024c3d74 and every instruction writing
// [reg+0x658], and decompile the functions that do it - one of them is the
// debug component's construction site, and whatever guards it is the final
// gate to patch.
public class DebugComponentCtor extends GhidraScript {
    PrintWriter out;
    DecompInterface dec;
    Set<String> done = new HashSet<>();

    void decomp(Function f, String why) {
        if (f == null || !done.add(f.getEntryPoint().toString())) return;
        out.println("############################################################");
        out.println("### " + why + " : " + f.getName() + " @ " + f.getEntryPoint());
        out.print("### callers:");
        int n = 0;
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            Function cf = getFunctionContaining(r.getFromAddress());
            if (cf != null) { out.print(" " + cf.getName()); if (++n > 8) { out.print(" ..."); break; } }
        }
        out.println();
        DecompileResults res = dec.decompileFunction(f, 120, new ConsoleTaskMonitor());
        if (res != null && res.decompileCompleted())
            out.println(res.getDecompiledFunction().getC());
        else out.println("  (decompile failed)");
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/debug_component_ctor.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        Address g = toAddr(0x24c3d74L);
        out.println("=== refs to DAT_024c3d74 ===");
        List<Function> writers = new ArrayList<>();
        for (Reference r : getReferencesTo(g)) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.println("  " + r.getFromAddress() + " " + r.getReferenceType()
                + (f == null ? "" : "  in " + f.getName() + "@" + f.getEntryPoint()));
            if (f != null && r.getReferenceType().isWrite()) writers.add(f);
        }
        for (Function f : writers) decomp(f, "writes DAT_024c3d74");

        // program-wide: writes of [reg+0x658]
        out.println("=== instructions writing [reg+0x658] ===");
        Listing lst = currentProgram.getListing();
        InstructionIterator ii = lst.getInstructions(true);
        List<Function> w658 = new ArrayList<>();
        while (ii.hasNext() && !monitor.isCancelled()) {
            Instruction ins = ii.next();
            String m = ins.getMnemonicString();
            if (!m.equals("MOV")) continue;
            String op0 = ins.getDefaultOperandRepresentation(0);
            if (op0 != null && op0.contains("[") && op0.contains("0x658")) {
                Function f = getFunctionContaining(ins.getAddress());
                out.println("  " + ins.getAddress() + ": " + ins
                    + (f == null ? "" : "   in " + f.getName()));
                if (f != null && !w658.contains(f)) w658.add(f);
            }
        }
        out.println("=== " + w658.size() + " distinct functions writing +0x658 ===");
        for (Function f : w658) decomp(f, "writes [+0x658]");
        out.close();
        println("done");
    }
}
