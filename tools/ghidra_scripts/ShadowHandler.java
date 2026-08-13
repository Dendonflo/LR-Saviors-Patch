import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// FrameworkDrawManager registers "DRAW_SHADOW" -> FUN_00ac6040. That handler
// runs the shadow pass, so the light projection is built in it or just below.
// Decompile it and everything it calls, one level deep.
public class ShadowHandler extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/shadow_handler.txt","UTF-8");
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        Set<String> seen = new LinkedHashSet<>();
        List<String> queue = new ArrayList<>(Arrays.asList("00ac6040"));
        for (int depth = 0; depth < 2; depth++) {
            List<String> next = new ArrayList<>();
            for (String a : queue) {
                Function f = getFunctionContaining(currentProgram.getAddressFactory().getAddress(a));
                if (f == null || !seen.add(f.getEntryPoint().toString())) continue;
                long sz = f.getBody().getNumAddresses();
                out.println("=== "+f.getName()+" @ "+f.getEntryPoint()+" size="+sz+" depth="+depth+" ===");
                if (sz > 8000) { out.println("  (too large)\n"); continue; }
                DecompileResults r = d.decompileFunction(f, 120, new ConsoleTaskMonitor());
                out.println(r!=null&&r.decompileCompleted()? r.getDecompiledFunction().getC():"  failed");
                out.println(); out.flush();
                for (Function c : f.getCalledFunctions(new ConsoleTaskMonitor()))
                    next.add(c.getEntryPoint().toString());
            }
            queue = next;
        }
        d.dispose(); out.close(); println("DONE");
    }
}
