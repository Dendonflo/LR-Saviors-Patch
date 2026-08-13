import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.*;

// The talk-manager timer step is a DOUBLE (SUBSD qword ptr [0x00df8fe0]), not a
// float - which is why the float sweeps missed it. Redo the sweep with correct
// typing: find every SUBSD/ADDSD/FSUB/FADD against a qword constant, resolve
// the constant as a double, and group by value.
//
// A hard-coded step subtracted from a field with no delta-time term is a
// frame-rate-dependent timer: it runs twice as fast at 60fps as at 30.
public class StepDoubleScan extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/step_double_scan.txt","UTF-8");

        // confirm the known site first
        out.println("######## known constants, read as double ########");
        String[] known = { "00df8fe0", "0207ac68", "00df5578", "0208e258", "02080d38" };
        for (String k : known) {
            Address a = currentProgram.getAddressFactory().getAddress(k);
            try {
                long bits = currentProgram.getMemory().getLong(a);
                out.println("  [" + k + "] double = " + Double.longBitsToDouble(bits)
                            + "   (0x" + Long.toHexString(bits) + ")");
            } catch (Exception ex) { out.println("  [" + k + "] unreadable"); }
        }
        out.println();

        Set<String> mn = new HashSet<>(Arrays.asList(
            "SUBSD","ADDSD","FSUB","FSUBR","FADD","FSUBP","FADDP","FSUBRP","COMISD","UCOMISD"));

        Map<Double,Map<String,List<String>>> byVal = new TreeMap<>();
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext()) {
            Instruction i = it.next();
            if (!mn.contains(i.getMnemonicString())) continue;
            if (!i.toString().contains("qword") && !i.toString().contains("double")) continue;
            for (Reference r : i.getReferencesFrom()) {
                if (!r.getReferenceType().isData()) continue;
                Address t = r.getToAddress();
                if (!currentProgram.getMemory().contains(t)) continue;
                double v;
                try { v = Double.longBitsToDouble(currentProgram.getMemory().getLong(t)); }
                catch (Exception ex) { continue; }
                if (Double.isNaN(v) || Double.isInfinite(v)) continue;
                double av = Math.abs(v);
                if (av == 0.0 || av > 2.0) continue;   // plausible per-frame step
                Function f = getFunctionContaining(i.getAddress());
                String fk = f == null ? "(none)" : f.getName() + " @ " + f.getEntryPoint();
                byVal.computeIfAbsent(v, k -> new LinkedHashMap<>())
                     .computeIfAbsent(fk, k -> new ArrayList<>())
                     .add("      " + i.getAddress() + "  " + i + "   const@" + t);
            }
        }

        out.println("######## double sub/add/compare against small constant ########\n");
        for (Map.Entry<Double,Map<String,List<String>>> e : byVal.entrySet()) {
            int total = 0;
            for (List<String> l : e.getValue().values()) total += l.size();
            out.println("==== " + e.getKey() + "  (" + e.getValue().size()
                        + " functions, " + total + " sites) ====");
            for (Map.Entry<String,List<String>> fe : e.getValue().entrySet()) {
                out.println("  " + fe.getKey());
                for (String l : fe.getValue()) out.println(l);
            }
            out.println();
        }
        out.close();
        println("DONE values=" + byVal.size());
    }
}
