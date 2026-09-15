import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// The FieldGlobal parameter table (0x20-byte records, 0x024c8a40..0x024c9b00,
// float at +0, name at +0x10) is walked by code near 0x005b2fd4 / 0x005b30c4
// (found by raw immediate scan; Ghidra has no xrefs because the static
// initialiser was never made a function). Decompile those functions and
// their callers: this is the WDB->table loader and/or the name lookup.
public class FieldGlobalReaders extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/field_global_readers.txt", "UTF-8");
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        Set<String> seen = new HashSet<>();
        for (String a : new String[]{"005b2fd4", "005b30c4", "005b28ee", "005b2af0", "005b2ce4", "005b2e5a"}) {
            Function f = getFunctionContaining(toAddr(Long.parseLong(a, 16)));
            if (f == null) { out.println("no function at " + a); continue; }
            if (!seen.add(f.getName())) continue;
            out.println("################ " + f.getName() + " @ " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses() + " (contains " + a + ") ################");
            DecompileResults r = dec.decompileFunction(f, 240, new ConsoleTaskMonitor());
            String c = (r != null && r.getDecompiledFunction() != null) ? r.getDecompiledFunction().getC() : "(failed)";
            if (c.length() > 20000) c = c.substring(0, 20000) + "\n... [truncated]";
            out.println(c);
            Set<String> callers = new TreeSet<>();
            for (Reference cr : getReferencesTo(f.getEntryPoint())) {
                Function cf = getFunctionContaining(cr.getFromAddress());
                if (cf != null) callers.add(cf.getName() + "@" + cr.getFromAddress());
            }
            out.println("---- callers (" + callers.size() + "): " + callers);
            out.println();
        }
        out.close();
        println("wrote field_global_readers.txt");
    }
}
