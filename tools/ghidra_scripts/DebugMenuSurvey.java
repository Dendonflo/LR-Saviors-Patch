import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Debug-menu survivability survey. XIII and XIII-2 shipped with a (hidden but
// mod-reachable) debug menu; the question is what LR still contains. String
// scan of the exe already showed 39 DebugMenuPage* RTTI classes, the LoadViewer
// suite, a debug terminal, and the telling window title
// "LIGHTNING RETURNS: FINAL FANTASY XIII (DebugComponentEnabled)".
//
// This script decompiles the functions containing each code reference to the
// gate-relevant strings, to find (a) the flag that decides DebugComponentEnabled,
// (b) whether the menu build/interval registration is intact or stubbed, and
// (c) what enables the debug font load (sys/debug/DebugFontTextureDDS.bin).
public class DebugMenuSurvey extends GhidraScript {

    DecompInterface dec;
    PrintWriter out;
    Set<String> done = new HashSet<>();

    void dump(long va, String why) {
        Address a = toAddr(va);
        Function f = getFunctionContaining(a);
        if (f == null) { out.println("### " + Long.toHexString(va) + " (" + why + "): NO FUNCTION"); return; }
        if (!done.add(f.getEntryPoint().toString())) {
            out.println("### " + Long.toHexString(va) + " (" + why + "): already dumped " + f.getName());
            return;
        }
        out.println("############################################################");
        out.println("### " + why + " -> ref at " + a + " inside " + f.getName() + " @ " + f.getEntryPoint());
        // callers, so the enable chain can be walked upward
        out.print("### callers:");
        int n = 0;
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            Function cf = getFunctionContaining(r.getFromAddress());
            if (cf != null) { out.print(" " + cf.getName() + "@" + cf.getEntryPoint()); if (++n > 12) { out.print(" ..."); break; } }
        }
        out.println();
        DecompileResults res = dec.decompileFunction(f, 90, new ConsoleTaskMonitor());
        if (res != null && res.decompileCompleted())
            out.println(res.getDecompiledFunction().getC());
        else
            out.println("  (decompile failed)");
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/debug_menu_survey.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        // code refs found by raw scan of the exe (VA base 0x400000 == Ghidra base)
        dump(0x42f6daL, "window title '(DebugComponentEnabled)'");
        dump(0x42aca6L, "loads sys/debug/DebugFontTextureDDS.bin");
        dump(0x42f202L, "registers INTERVAL_DEBUG_MENU_WHITE");
        dump(0x8cdcf7L, "uses 'PrototypeDebugMenu (%s)'");
        dump(0x666cdbL, "'---- DEBUG CAMERA ----'");
        dump(0x42970cL, "White.Debug ref 1");
        dump(0x42f736L, "White.Debug ref 2 (near title)");
        dump(0x434b40L, "White.Debug ref 3");

        out.close();
        println("done");
    }
}
