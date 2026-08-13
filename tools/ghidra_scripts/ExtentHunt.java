import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// Extent candidates called from inside FUN_00a32a00 (the shadow render pass),
// around where the cascade matrices are built and the two cascade surfaces
// are bound. Looking for the ortho half-width of the light frustum.
public class ExtentHunt extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/extent_hunt.txt","UTF-8");
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        for (String a : new String[]{"00a31da0","00a31990","00a31770","00b03780","00a325e0"}) {
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
