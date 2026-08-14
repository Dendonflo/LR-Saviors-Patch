import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Core of the debug menu: FUN_0065dc30 (per-frame tick behind manager vslot
// +0x84) and FUN_00666400 (init behind +0x40). These should show the enable
// bit checks (byte+6 & 2/4), the input that opens the menu, and the page
// registration - the last unknowns before writing the survey summary.
public class DebugMenuCore extends GhidraScript {
    PrintWriter out;
    DecompInterface dec;
    Set<String> done = new HashSet<>();

    void decomp(Function f, String why) {
        if (f == null) { out.println("### (" + why + "): null"); return; }
        if (!done.add(f.getEntryPoint().toString())) return;
        out.println("############################################################");
        out.println("### " + why + " : " + f.getName() + " @ " + f.getEntryPoint());
        DecompileResults res = dec.decompileFunction(f, 120, new ConsoleTaskMonitor());
        if (res != null && res.decompileCompleted())
            out.println(res.getDecompiledFunction().getC());
        else out.println("  (decompile failed)");
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/debug_menu_core.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        decomp(getFunctionAt(toAddr(0x65dc30L)), "debug menu per-frame tick core");
        decomp(getFunctionAt(toAddr(0x666400L)), "debug menu init core");
        out.close();
        println("done");
    }
}
