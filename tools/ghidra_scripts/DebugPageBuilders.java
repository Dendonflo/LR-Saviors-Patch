import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Raw-binary RTTI walk (Ghidra's own rttiRefs were a false negative - its
// RTTI analyzer never ran) found 51 code sites that install DebugMenuPage
// vtables, clustered into a handful of functions:
//   0x40a8d2..0x40b474   (EditSphere, Level)
//   0x762c62, 0x763636..0x763b16  (CharaPop, Common, Effect*, ShadowConfig,
//                                  WeatherConfig, ColorCorrection, Field)
//   0x764aaa..0x7659b2   (FieldAi*, FieldEffect*, GreyConfig, Gui, Lay*)
//   0x76872b..0x76882e   (PhysicsMain, Sound)
// So the pages ARE constructed by real code. The remaining question is
// whether that code is REACHABLE: resolve the containing functions and walk
// callers upward until we hit either a known-live boot path or a dead end.
public class DebugPageBuilders extends GhidraScript {
    PrintWriter out;
    DecompInterface dec;

    // walk callers breadth-first up to `depth` levels
    void walkUp(Function f, int depth, String indent, Set<String> seen) {
        if (f == null || depth == 0) return;
        List<String> cs = new ArrayList<>();
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            Function cf = getFunctionContaining(r.getFromAddress());
            if (cf != null) cs.add(cf.getName() + "@" + cf.getEntryPoint());
            else cs.add("(DATA/vtable slot @" + r.getFromAddress() + ")");
        }
        if (cs.isEmpty()) { out.println(indent + "^ NO CALLERS (dead end / entry via vtable only)"); return; }
        for (String c : cs) {
            out.println(indent + "^ " + c);
            if (c.startsWith("(")) continue;
            if (!seen.add(c)) { out.println(indent + "  (already walked)"); continue; }
            Address a = toAddr(Long.parseLong(c.substring(c.indexOf('@') + 1), 16));
            walkUp(getFunctionAt(a), depth - 1, indent + "  ", seen);
        }
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/debug_page_builders.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        long[] sites = { 0x40a8d2L, 0x40b07bL, 0x762c62L, 0x763636L, 0x763692L, 0x763a02L,
                         0x763b16L, 0x764aaaL, 0x764f8fL, 0x7650b8L, 0x765219L, 0x76588bL,
                         0x76872bL, 0x768812L, 0x7428f6L /* HID iface */,
                         0x76f87cL /* terminal Shell */, 0x40e0ebL /* CheatPadWindow */ };
        Set<String> builders = new LinkedHashSet<>();
        out.println("================ containing functions of the page-ctor sites ================");
        for (long s : sites) {
            Function f = getFunctionContaining(toAddr(s));
            out.println("  site " + Long.toHexString(s) + " -> "
                + (f == null ? "NO FUNCTION" : f.getName() + "@" + f.getEntryPoint()
                   + " size=" + f.getBody().getNumAddresses()));
            if (f != null) builders.add(f.getName() + "@" + f.getEntryPoint());
        }

        out.println();
        out.println("================ caller chains (upward, 4 levels) ================");
        for (String b : builders) {
            Address a = toAddr(Long.parseLong(b.substring(b.indexOf('@') + 1), 16));
            out.println("### " + b);
            walkUp(getFunctionAt(a), 4, "  ", new HashSet<String>());
            out.println();
        }

        // decompile the main page-registration builder for its gating
        out.println("================ decompile: main page builder ================");
        Function main = getFunctionContaining(toAddr(0x763692L));
        if (main != null) {
            DecompileResults res = dec.decompileFunction(main, 180, new ConsoleTaskMonitor());
            if (res != null && res.decompileCompleted())
                out.println(res.getDecompiledFunction().getC());
        }
        out.close();
        println("done");
    }
}
