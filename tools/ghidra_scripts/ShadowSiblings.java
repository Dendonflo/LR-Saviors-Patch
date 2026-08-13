import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// Siblings of the render pass in the shadow-system class (00a31xxx-00a33xxx).
// One of these should be the per-frame fit/update that positions the cascades.
public class ShadowSiblings extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/shadow_siblings.txt","UTF-8");
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        for (String a : new String[]{"00a32800","00a33260","00a33420","00a32460","00a31bc0","00a32120"}) {
            Function f = getFunctionContaining(currentProgram.getAddressFactory().getAddress(a));
            if (f == null) continue;
            out.println("=== "+a+" -> "+f.getName()+" size="+f.getBody().getNumAddresses()+" ===");
            DecompileResults r = d.decompileFunction(f, 150, new ConsoleTaskMonitor());
            out.println(r!=null&&r.decompileCompleted()? r.getDecompiledFunction().getC():"  failed");
            out.println(); out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
