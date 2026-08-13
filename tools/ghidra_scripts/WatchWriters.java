import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.*;

// Hardware-watchpoint capture named the instructions that write to the plot
// name buffer. Resolve each to its containing function, and pull the embedded
// .cpp source path where the function (or a callee) asserts - the exe carries
// 324 original source paths, which is the fastest way to say what a function
// actually is.
//
// A data breakpoint traps AFTER the store, so each EIP is the instruction
// following the write; getFunctionContaining still lands in the right function.
public class WatchWriters extends GhidraScript {
    private String srcOf(Function f) {
        if (f == null) return "";
        Set<String> hits = new LinkedHashSet<>();
        InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
        int guard = 0;
        while (it.hasNext() && guard++ < 3000) {
            Instruction i = it.next();
            for (Reference r : i.getReferencesFrom()) {
                Data d = getDataAt(r.getToAddress());
                if (d != null && d.hasStringValue()) {
                    Object v = d.getValue();
                    if (v == null) continue;
                    String s = v.toString();
                    if (s.contains(".cpp")) {
                        int k = s.lastIndexOf('\\');
                        hits.add(k >= 0 ? s.substring(k + 1) : s);
                    }
                }
            }
        }
        return hits.isEmpty() ? "" : String.join(",", hits);
    }

    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/watch_writers.txt","UTF-8");
        String[] eips = {
            "7467D694","0070B435","0065E538","007EE370","006C26D0","007106A0","006C3C00",
            "0072169D","00721720","00721734","00721742","00721749","00721773","00721799",
            "00721834","00721934","00721A0D","00721A5F","00721A68","00721A6D","00721A7A",
            "00721A7F"
        };
        Map<String,List<String>> byFunc = new LinkedHashMap<>();
        for (String e : eips) {
            String label;
            try {
                Address a = currentProgram.getAddressFactory().getAddress(e);
                Function f = getFunctionContaining(a);
                if (f == null) { label = "(outside module / no function)"; }
                else {
                    String src = srcOf(f);
                    label = f.getName() + " @ " + f.getEntryPoint()
                            + (src.isEmpty() ? "" : "   [" + src + "]");
                }
            } catch (Exception ex) { label = "(unresolvable)"; }
            byFunc.computeIfAbsent(label, k -> new ArrayList<>()).add(e);
        }
        out.println("writers to the plot-name buffer, grouped by function:\n");
        for (Map.Entry<String,List<String>> en : byFunc.entrySet()) {
            out.println("  " + en.getKey());
            out.println("      eips: " + String.join(" ", en.getValue()));
        }
        out.close();
        println("DONE");
    }
}
