import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// r_field_global.wdb ("FieldGlobal" sheet: POPPopLength, FEPopRange, ...) is
// mirrored into a static table of 0x20-byte records in .data around
// 0x024c8d00..0x024c9700 (float value at +0, name at +0x10), built by an
// unrolled static initialiser at 0x00dc0000.. that Ghidra never made a
// function of. Find every function that references INTO that range - the
// WDB loader that overwrites the defaults, and the getter the field code
// reads through - so the live NPC/enemy pop distance can be located and
// written at runtime.
public class FieldGlobalTable extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/field_global_table.txt", "UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        long lo = 0x024c8d00L, hi = 0x024c9800L;
        Map<String, List<String>> byFunc = new TreeMap<>();
        List<String> orphan = new ArrayList<>();
        ReferenceIterator it = currentProgram.getReferenceManager()
            .getReferenceIterator(toAddr(lo));
        while (it.hasNext()) {
            Reference r = it.next();
            if (r.getToAddress().getOffset() >= hi) break;
            Function f = getFunctionContaining(r.getFromAddress());
            String line = r.getFromAddress() + " -> " + r.getToAddress() + " " + r.getReferenceType();
            if (f == null) orphan.add(line);
            else byFunc.computeIfAbsent(f.getName() + " @ " + f.getEntryPoint(), k -> new ArrayList<>()).add(line);
        }
        out.println("orphan refs (static initialiser, no function): " + orphan.size());
        if (!orphan.isEmpty()) { out.println("  first " + orphan.get(0) + "  last " + orphan.get(orphan.size()-1)); }
        out.println("functions referencing the table: " + byFunc.size());
        for (Map.Entry<String, List<String>> e : byFunc.entrySet()) {
            out.println("  " + e.getKey() + "  (" + e.getValue().size() + " refs)");
            for (int i = 0; i < Math.min(6, e.getValue().size()); i++) out.println("      " + e.getValue().get(i));
        }
        out.println();
        for (String k : byFunc.keySet()) {
            Function f = getFunctionAt(toAddr(Long.parseLong(k.substring(k.indexOf('@') + 2), 16)));
            if (f == null || f.getBody().getNumAddresses() > 6000) { out.println("#### " + k + " skipped (size)"); continue; }
            out.println("################ " + k + " size=" + f.getBody().getNumAddresses() + " ################");
            DecompileResults r = dec.decompileFunction(f, 240, new ConsoleTaskMonitor());
            String c = (r != null && r.getDecompiledFunction() != null) ? r.getDecompiledFunction().getC() : "(failed)";
            if (c.length() > 14000) c = c.substring(0, 14000) + "\n... [truncated]";
            out.println(c); out.println();
            // callers, one level
            Set<String> callers = new TreeSet<>();
            for (Reference cr : getReferencesTo(f.getEntryPoint())) {
                Function cf = getFunctionContaining(cr.getFromAddress());
                if (cf != null) callers.add(cf.getName());
            }
            out.println("---- callers of " + f.getName() + " (" + callers.size() + "): " + callers);
            out.println();
        }
        out.close();
        println("wrote field_global_table.txt");
    }
}
