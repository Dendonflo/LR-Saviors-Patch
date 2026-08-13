import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// The settings/allocator path was a dead end for EXTENT. Going at the render
// side: the engine has literal "DRAW_SHADOW_BEGIN"/"DRAW_SHADOW"/
// "DRAW_SHADOW_END" markers (referenced by FUN_00b03b00 / FUN_00ac72d0) and a
// "shadowMap" reference in FUN_00c5e890. Whoever draws the shadow pass must
// set up the light projection immediately beforehand.
public class DrawShadow extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/draw_shadow.txt","UTF-8");
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        for (String a : new String[]{"00ac72d0","00b03b00","00c5e890","00784cd0"}) {
            Function f = getFunctionContaining(currentProgram.getAddressFactory().getAddress(a));
            if (f == null) { out.println("=== "+a+" none ===\n"); continue; }
            out.println("=== "+a+" -> "+f.getName()+" size="+f.getBody().getNumAddresses()+" ===");
            DecompileResults r = d.decompileFunction(f, 120, new ConsoleTaskMonitor());
            out.println(r!=null&&r.decompileCompleted()? r.getDecompiledFunction().getC():"  failed");
            out.println(); out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
