import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.data.StringDataType;
import java.io.PrintWriter;
import java.util.*;

// Two questions at once:
//
// 1. Are shadows applied in a HALF-RESOLUTION screen-space pass? The RT
//    inventory at 3840x2160 shows 2x A8R8G8B8 + 1x R32F at exactly 1920x1080,
//    but half-res is equally standard for SSAO, volumetrics and half-res
//    transparency - the dimensions alone cannot say which.
//
// 2. Is this renderer forward or deferred? An earlier conclusion of "deferred,
//    MSAA off the table" rested on max SetRenderTarget index = 3 (i.e. MRT).
//    That proves MRT is used, which is NOT the same as deferred shading - a
//    light-pre-pass/deferred-lighting renderer also uses MRT for a depth+normal
//    prepass and then shades FORWARD.
//
// The draw-pass registration table answers both by naming every pass. DRAW_SHADOW
// is known to live in it and map to FUN_00ac6040, so the neighbours are the rest
// of the frame graph. Find every "DRAW_*" string, then the code referencing it,
// and the handler address registered alongside.
public class DrawPassTable extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/draw_pass_table.txt","UTF-8");

        Listing lst = currentProgram.getListing();
        // Collect every defined string that looks like a pass name.
        List<Object[]> hits = new ArrayList<>();
        DataIterator di = lst.getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            if (!(d.getDataType() instanceof StringDataType)) continue;
            Object v = d.getValue();
            if (v == null) continue;
            String s = v.toString();
            if (s.length() < 3 || s.length() > 64) continue;
            String u = s.toUpperCase();
            if (!(u.startsWith("DRAW") || u.contains("SHADOW") || u.contains("SSAO")
                  || u.contains("DEFERRED") || u.contains("GBUFFER") || u.contains("G_BUFFER")
                  || u.contains("LIGHT") || u.contains("FORWARD") || u.contains("OPAQUE")
                  || u.contains("TRANSLUCENT") || u.contains("HALF"))) continue;
            hits.add(new Object[]{ d.getAddress(), s });
        }
        out.println("################ candidate pass / render strings: " + hits.size() + " ################");
        for (Object[] h : hits) {
            Address a = (Address) h[0];
            String s = (String) h[1];
            StringBuilder refs = new StringBuilder();
            int n = 0;
            for (Reference r : getReferencesTo(a)) {
                if (n++ > 4) { refs.append(" ..."); break; }
                Function f = getFunctionContaining(r.getFromAddress());
                refs.append("  ").append(r.getFromAddress())
                    .append(f == null ? "" : "(" + f.getName() + ")");
            }
            out.printf("%-12s %-46s refs:%s%n", a, "\"" + s + "\"",
                       refs.length() == 0 ? " (none)" : refs.toString());
        }
        out.close();
        println("wrote draw_pass_table.txt (" + hits.size() + " strings)");
    }
}
