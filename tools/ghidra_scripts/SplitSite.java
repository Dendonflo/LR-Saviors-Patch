import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Runtime stack scan pinned the shadowSplitRange upload to 00A3723F /
// 00A372D4 / 00A372F6 - present in every capture. Decompile that function
// and its callers; the fit that produced (10.0, 79.2) is there.
public class SplitSite extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/split_site.txt","UTF-8");
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        Set<String> done = new LinkedHashSet<>();
        List<String> seeds = new ArrayList<>(Arrays.asList("00A3723F","00A372D4","00A372F6"));
        for (String a : seeds) {
            Function f = getFunctionContaining(currentProgram.getAddressFactory().getAddress(a));
            if (f == null) { out.println("=== "+a+" no function ===\n"); continue; }
            String k = f.getEntryPoint().toString();
            out.println("### scan addr "+a+" is inside "+f.getName()+" @ "+k);
            if (!done.add(k)) { out.println("   (already dumped)\n"); continue; }
            out.println("=== "+f.getName()+" @ "+k+" size="+f.getBody().getNumAddresses()+" ===");
            DecompileResults r = d.decompileFunction(f, 180, new ConsoleTaskMonitor());
            out.println(r!=null&&r.decompileCompleted()? r.getDecompiledFunction().getC():" failed");
            out.println(); out.flush();
            for (Reference ref : getReferencesTo(f.getEntryPoint())) {
                Function c = getFunctionContaining(ref.getFromAddress());
                if (c == null || !done.add(c.getEntryPoint().toString())) continue;
                out.println("=== CALLER "+c.getName()+" @ "+c.getEntryPoint()
                            +" size="+c.getBody().getNumAddresses()+" ===");
                if (c.getBody().getNumAddresses() > 9000) { out.println("  (too large)\n"); continue; }
                DecompileResults r2 = d.decompileFunction(c, 180, new ConsoleTaskMonitor());
                out.println(r2!=null&&r2.decompileCompleted()? r2.getDecompiledFunction().getC():" failed");
                out.println(); out.flush();
            }
        }
        d.dispose(); out.close(); println("DONE");
    }
}
