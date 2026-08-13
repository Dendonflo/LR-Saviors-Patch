import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Runtime _ReturnAddress() pinned the shadowSplitRange upload to 00A84EFE.
// Decompile that function and walk its callers - the fit that produced
// (10.0, 79.2) is there or one level up.
public class UploadSite extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/upload_site.txt","UTF-8");
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        Set<String> done = new LinkedHashSet<>();
        Function f = getFunctionContaining(currentProgram.getAddressFactory().getAddress("00A84EFE"));
        out.println("### 00A84EFE is inside " + f.getName() + " @ " + f.getEntryPoint()
                    + " (offset +0x" + Long.toHexString(0xA84EFEL - f.getEntryPoint().getOffset()) + ")");
        List<Function> layer = new ArrayList<>(Arrays.asList(f));
        for (int depth = 0; depth < 3 && !layer.isEmpty(); depth++) {
            List<Function> next = new ArrayList<>();
            for (Function g : layer) {
                if (!done.add(g.getEntryPoint().toString())) continue;
                long sz = g.getBody().getNumAddresses();
                out.println("=== depth"+depth+" "+g.getName()+" @ "+g.getEntryPoint()+" size="+sz+" ===");
                if (sz > 9000) { out.println("  (too large)\n"); }
                else {
                    DecompileResults r = d.decompileFunction(g, 180, new ConsoleTaskMonitor());
                    out.println(r!=null&&r.decompileCompleted()? r.getDecompiledFunction().getC():" failed");
                    out.println();
                }
                out.flush();
                for (Reference ref : getReferencesTo(g.getEntryPoint())) {
                    Function c = getFunctionContaining(ref.getFromAddress());
                    if (c != null) next.add(c);
                }
            }
            layer = next;
        }
        d.dispose(); out.close(); println("DONE");
    }
}
