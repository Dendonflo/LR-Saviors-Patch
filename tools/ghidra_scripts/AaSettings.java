import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.data.StringDataType;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Two goals.
//
// 1. FXAA. The game's built-in FXAA is not togglable in its menus and smears
//    the image; it is also the prerequisite for evaluating any other AA, since
//    otherwise they stack. Before writing shader interception, check whether
//    the ENGINE has its own switch - that is how ShadowMapRes was found, where
//    the Graphics_Shadowing debug handlers turned out to be two-liners writing
//    a settings field, and writing that field was the entire fix.
//
// 2. The whole graphics settings surface. Graphics_Shadowing was found almost
//    by accident; dumping every Graphics_* / debug-menu setting name with its
//    referencing code shows what else is switchable the same cheap way.
//
// Also scans for AA-specific vocabulary (FXAA, antialias, edge/smooth/blur
// filter) and for the DRAW_FILTER handler's neighbourhood, since FXAA in this
// frame graph would live in or around that pass.
public class AaSettings extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/aa_settings.txt","UTF-8");
        Listing lst = currentProgram.getListing();

        String[] pats = { "FXAA", "ANTIALIAS", "ANTI_ALIAS", "ANTI-ALIAS", "AALEVEL",
                          "MULTISAMPLING", "EDGE", "SMOOTH", "GRAPHICS_", "DEBUG_MENU",
                          "POSTFILTER", "POST_FILTER", "SHARPEN", "LUMA" };
        out.println("################ AA / settings vocabulary ################");
        DataIterator di = lst.getDefinedData(true);
        int hits = 0;
        while (di.hasNext()) {
            Data d = di.next();
            if (!(d.getDataType() instanceof StringDataType)) continue;
            Object v = d.getValue();
            if (v == null) continue;
            String s = v.toString();
            if (s.length() < 3 || s.length() > 80) continue;
            String u = s.toUpperCase();
            boolean match = false;
            for (String p : pats) if (u.contains(p)) { match = true; break; }
            if (!match) continue;
            StringBuilder refs = new StringBuilder();
            int n = 0;
            for (Reference r : getReferencesTo(d.getAddress())) {
                if (n++ > 3) { refs.append(" ..."); break; }
                Function f = getFunctionContaining(r.getFromAddress());
                refs.append("  ").append(r.getFromAddress())
                    .append(f == null ? "" : "(" + f.getName() + ")");
            }
            out.printf("%-12s %-46s%s%n", d.getAddress(), "\"" + s + "\"",
                       refs.length() == 0 ? "  (no refs)" : refs.toString());
            hits++;
        }
        out.println("(" + hits + " matches)");

        // DRAW_FILTER's handler - the post/AA stage in this engine's frame graph.
        out.println();
        out.println("################ DRAW_FILTER handler FUN_00ac7030 ################");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Address a = currentProgram.getAddressFactory().getAddress("00ac7030");
        Function f = getFunctionContaining(a);
        if (f != null) {
            DecompileResults r = dec.decompileFunction(f, 180, new ConsoleTaskMonitor());
            String c = (r != null && r.getDecompiledFunction() != null)
                       ? r.getDecompiledFunction().getC() : "(failed)";
            if (c.length() > 10000) c = c.substring(0, 10000) + "\n... [truncated]";
            out.println(c);
        }
        out.close();
        println("wrote aa_settings.txt (" + hits + " string matches)");
    }
}
