import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// The render pass only CONSUMES cascade state. The extent is set by a
// per-frame update/fit step that writes the cascade array at
// [shadowSystem + 0x63bc] and the neighbouring fields the render reads
// (+0x61f0, +0x670c, +0x6c10, +0x6da4).
//
// Those offsets are large and distinctive, so scan the whole text section for
// instructions carrying them as displacements - that finds every function
// touching the shadow system object regardless of how it was reached.
public class CascadeFit extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/cascade_fit.txt","UTF-8");
        long[] offs = {0x63bcL, 0x61f0L, 0x670cL, 0x6c10L, 0x6da4L, 0x63b4L};
        Set<Long> want = new HashSet<>();
        for (long o : offs) want.add(o);

        Map<String, Set<Long>> hits = new LinkedHashMap<>();
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            for (int i = 0; i < ins.getNumOperands(); i++) {
                for (Object o : ins.getOpObjects(i)) {
                    if (!(o instanceof Scalar)) continue;
                    long v = ((Scalar)o).getUnsignedValue();
                    if (!want.contains(v)) continue;
                    Function f = getFunctionContaining(ins.getAddress());
                    if (f == null) continue;
                    hits.computeIfAbsent(f.getEntryPoint().toString(), k -> new TreeSet<>()).add(v);
                }
            }
        }
        out.println("======== functions touching shadow-system offsets ========");
        for (Map.Entry<String, Set<Long>> e : hits.entrySet()) {
            Function f = getFunctionContaining(currentProgram.getAddressFactory().getAddress(e.getKey()));
            StringBuilder sb = new StringBuilder();
            for (long v : e.getValue()) sb.append(String.format("0x%x ", v));
            out.println(String.format("  %-14s size=%-6d offsets: %s",
                        e.getKey(), f.getBody().getNumAddresses(), sb));
        }
        out.println();
        out.flush();

        // decompile the ones touching the cascade array itself, smallest first
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        List<String> arr = new ArrayList<>();
        for (Map.Entry<String, Set<Long>> e : hits.entrySet())
            if (e.getValue().contains(0x63bcL) || e.getValue().contains(0x63b4L)) arr.add(e.getKey());
        arr.sort((x,y) -> Long.compare(
            getFunctionContaining(currentProgram.getAddressFactory().getAddress(x)).getBody().getNumAddresses(),
            getFunctionContaining(currentProgram.getAddressFactory().getAddress(y)).getBody().getNumAddresses()));
        int n = 0;
        for (String k : arr) {
            if (++n > 8) break;
            Function f = getFunctionContaining(currentProgram.getAddressFactory().getAddress(k));
            if (f.getBody().getNumAddresses() > 9000) { out.println("=== "+k+" too large ===\n"); continue; }
            out.println("=== "+f.getName()+" @ "+k+" size="+f.getBody().getNumAddresses()+" ===");
            DecompileResults r = d.decompileFunction(f, 150, new ConsoleTaskMonitor());
            out.println(r!=null&&r.decompileCompleted()? r.getDecompiledFunction().getC():"  failed");
            out.println(); out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
