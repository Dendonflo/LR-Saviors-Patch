import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.*;

// FUN_009e3670 is what stamps its SEH handler address into the stack slot the
// Gysahl plot-name buffer used to occupy. Identify what it actually IS.
//
// Two identification routes:
//  1. The exe embeds 324 original .cpp source paths in its assert calls, so
//     any asserting function maps back to its source file. Walk each target
//     and its callees looking for a path string.
//  2. FUN_009e3b70 is referenced as DATA from 0231ec30 - a table slot, not a
//     call. Dump the neighbourhood of that table; if it is a dispatch/vtable
//     the surrounding entries say what family it belongs to.
public class IdentifyVmFuncs extends GhidraScript {

    private void sourcePaths(PrintWriter out, String addr, String label) throws Exception {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        Function f = getFunctionContaining(a);
        if (f == null) { out.println(label + ": no function"); return; }
        out.println("==== " + label + "  " + f.getName() + " @ " + f.getEntryPoint()
                    + " size=" + f.getBody().getNumAddresses() + " ====");
        Set<String> found = new LinkedHashSet<>();
        InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
        while (it.hasNext()) {
            Instruction i = it.next();
            for (Reference r : i.getReferencesFrom()) {
                Data d = getDataAt(r.getToAddress());
                if (d != null && d.hasStringValue()) {
                    Object v = d.getValue();
                    if (v == null) continue;
                    String s = v.toString();
                    if (s.contains("\\") || s.contains(".cpp") || s.length() > 6) found.add(s);
                }
            }
        }
        if (found.isEmpty()) out.println("   (no strings referenced directly)");
        for (String s : found) out.println("   STR: " + s);

        // one level down: callees often carry the assert with the path
        Set<String> callees = new LinkedHashSet<>();
        it = currentProgram.getListing().getInstructions(f.getBody(), true);
        while (it.hasNext()) {
            Instruction i = it.next();
            for (Reference r : i.getReferencesFrom()) {
                if (!r.getReferenceType().isCall()) continue;
                Function c = getFunctionAt(r.getToAddress());
                if (c == null) continue;
                InstructionIterator ci = currentProgram.getListing().getInstructions(c.getBody(), true);
                int guard = 0;
                while (ci.hasNext() && guard++ < 400) {
                    Instruction x = ci.next();
                    for (Reference xr : x.getReferencesFrom()) {
                        Data xd = getDataAt(xr.getToAddress());
                        if (xd != null && xd.hasStringValue()) {
                            Object v = xd.getValue();
                            if (v == null) continue;
                            String s = v.toString();
                            if (s.contains(".cpp")) callees.add(c.getName() + "  ->  " + s);
                        }
                    }
                }
            }
        }
        for (String s : callees) out.println("   CALLEE SRC: " + s);
        out.println();
    }

    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/identify_vm_funcs.txt","UTF-8");
        sourcePaths(out, "009e3670", "THE CLOBBERER");
        sourcePaths(out, "009e3b70", "table-referenced caller");
        sourcePaths(out, "009e45c0", "caller of the clobberer");
        sourcePaths(out, "009e34c0", "yield primitive");

        out.println("==== table at 0231ec30 (references FUN_009e3b70) ====");
        Address t = currentProgram.getAddressFactory().getAddress("0231ec30");
        for (int off = -0x20; off <= 0x20; off += 4) {
            try {
                Address p = t.add(off);
                int v = currentProgram.getMemory().getInt(p);
                Address tgt = currentProgram.getAddressFactory().getAddress(
                    String.format("%08x", v));
                Function tf = getFunctionAt(tgt);
                String note = tf != null ? ("  -> " + tf.getName()) : "";
                Data dd = getDataAt(tgt);
                if (tf == null && dd != null && dd.hasStringValue())
                    note = "  -> STR '" + dd.getValue() + "'";
                out.println(String.format("  %s : %08X%s", p, v, note));
            } catch (Exception e) {}
        }
        out.close();
        println("DONE");
    }
}
