import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Most debug-menu page-id strings ("debug_menu_common", "debug_menu_field",
// "debug_menu_battle", ...) are referenced by NOTHING - verified by raw
// pointer search, not just Ghidra. But a handful DO have code references:
//   debug_menu_cdev_lay_inst_list -> FUN_00765810
//   debug_menu_cdev_lay_denv_list -> FUN_00765840
//   debug_battle_menu             -> FUN_007abd90
//   DRAW_DEBUG_LOADVIEWER_WHITE   -> FUN_007741f0
// These are the only surviving threads of registration code. Decompile them
// and walk their callers: if even these terminate in unreachable clusters,
// the registration layer is definitively gone and no flag can revive the menu.
public class DebugSurvivors extends GhidraScript {
    PrintWriter out;
    DecompInterface dec;
    Set<String> dumped = new HashSet<>();

    void walkUp(Function f, int depth, String indent, Set<String> seen) {
        if (f == null || depth == 0) return;
        int n = 0;
        boolean any = false;
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            any = true;
            Function cf = getFunctionContaining(r.getFromAddress());
            if (cf == null) { out.println(indent + "^ (DATA @" + r.getFromAddress() + ")"); continue; }
            String key = cf.getName() + "@" + cf.getEntryPoint();
            out.println(indent + "^ " + key);
            if (seen.add(key)) walkUp(cf, depth - 1, indent + "  ", seen);
            if (++n > 6) { out.println(indent + "  ..."); break; }
        }
        if (!any) out.println(indent + "^ NO REFERENCES AT ALL (unreachable)");
    }

    void dump(long va, String why) {
        Function f = getFunctionContaining(toAddr(va));
        if (f == null) { out.println("### " + why + ": no function at " + Long.toHexString(va)); return; }
        out.println("############################################################");
        out.println("### " + why + " : " + f.getName() + " @ " + f.getEntryPoint());
        out.println("--- caller chain:");
        walkUp(f, 5, "  ", new HashSet<String>());
        if (dumped.add(f.getEntryPoint().toString())) {
            DecompileResults res = dec.decompileFunction(f, 120, new ConsoleTaskMonitor());
            if (res != null && res.decompileCompleted())
                out.println(res.getDecompiledFunction().getC());
        }
        out.println();
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/debug_survivors.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        dump(0x765810L, "uses id 'debug_menu_cdev_lay_inst_list'");
        dump(0x765840L, "uses id 'debug_menu_cdev_lay_denv_list'");
        dump(0x7abd90L, "uses id 'debug_battle_menu'");
        dump(0x7741f0L, "registers DRAW_DEBUG_LOADVIEWER_WHITE");
        dump(0x742750L, "DebugMenuHidInterface vtable site");
        dump(0x76f830L, "terminal Shell ctor");
        dump(0x8cdca0L, "PrototypeDebugMenu thread spawner");

        out.close();
        println("done");
    }
}
