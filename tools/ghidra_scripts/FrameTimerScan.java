import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryAccessException;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.RefType;
import java.io.PrintWriter;
import java.util.*;

// FieldTalkManager case 10 does:  timer = timer - 0.05;  if (timer <= 0) advance;
// No delta-time term -> the countdown runs twice as fast at 60fps as at 30.
// That is the whole 60fps bug class. Sweep the binary for the same signature:
// a scalar float sub/add against a small .rdata constant.
//
// Correct (frame-rate independent) code multiplies by a delta global instead,
// so also report whether the enclosing function touches a MULSS/FMUL against a
// non-constant memory operand - that is the cheap "is it scaled?" tell.
public class FrameTimerScan extends GhidraScript {

    private Float readFloat(Address a) {
        try { return Float.intBitsToFloat(currentProgram.getMemory().getInt(a)); }
        catch (Exception e) { return null; }
    }

    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/frame_timer_scan.txt","UTF-8");

        // --- part 1: who calls the talk manager update? (is it per-frame?) ---
        out.println("######## callers of FUN_005de2c0 (FieldTalkManager update) ########");
        Address tm = currentProgram.getAddressFactory().getAddress("005de2c0");
        for (Reference r : getReferencesTo(tm)) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.println("  from " + r.getFromAddress() + " " + r.getReferenceType()
                        + (f == null ? "" : "  in " + f.getName() + " @ " + f.getEntryPoint()
                                            + " size=" + f.getBody().getNumAddresses()));
        }
        out.println();

        // --- part 2: binary-wide scan for unscaled float steps ---
        Set<String> subMnem = new HashSet<>(Arrays.asList(
            "SUBSS","ADDSS","FSUB","FSUBR","FADD","FSUBP","FADDP","SUBSD","ADDSD"));
        // functionEntry -> list of findings
        Map<String,List<String>> hits = new LinkedHashMap<>();
        Map<String,Boolean> scaled = new HashMap<>();

        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        int n = 0;
        while (it.hasNext()) {
            Instruction ins = it.next();
            n++;
            String m = ins.getMnemonicString();
            Function f = getFunctionContaining(ins.getAddress());
            if (f == null) continue;
            String fk = f.getName() + " @ " + f.getEntryPoint();

            // does this function scale anything by a memory (non-immediate) float?
            if (m.equals("MULSS") || m.equals("FMUL") || m.equals("MULSD")) {
                for (Reference r : ins.getReferencesFrom()) {
                    if (r.getReferenceType().isData()) { scaled.put(fk, true); break; }
                }
            }

            if (!subMnem.contains(m)) continue;
            // operand must reference a static memory location holding a float
            for (Reference r : ins.getReferencesFrom()) {
                if (!r.getReferenceType().isData()) continue;
                Address t = r.getToAddress();
                if (!currentProgram.getMemory().contains(t)) continue;
                // only initialized, read-only-ish data (a literal pool constant)
                Float v = readFloat(t);
                if (v == null || v.isNaN() || v.isInfinite()) continue;
                float av = Math.abs(v);
                if (av == 0.0f || av > 5.0f) continue;      // plausible per-step magnitude
                hits.computeIfAbsent(fk, k -> new ArrayList<>())
                    .add("    " + ins.getAddress() + "  " + ins + "   const[" + t + "] = " + v);
            }
        }

        out.println("######## scanned " + n + " instructions ########");
        out.println("######## float sub/add against small constant, grouped by function ########");
        out.println("(UNSCALED = function never multiplies by a memory float -> prime suspect)\n");

        List<Map.Entry<String,List<String>>> es = new ArrayList<>(hits.entrySet());
        // unscaled first, then by hit count
        es.sort((x,y) -> {
            boolean sx = scaled.getOrDefault(x.getKey(), false);
            boolean sy = scaled.getOrDefault(y.getKey(), false);
            if (sx != sy) return sx ? 1 : -1;
            return y.getValue().size() - x.getValue().size();
        });
        for (Map.Entry<String,List<String>> e : es) {
            boolean s = scaled.getOrDefault(e.getKey(), false);
            out.println((s ? "[scaled ] " : "[UNSCALED] ") + e.getKey()
                        + "  (" + e.getValue().size() + ")");
            for (String s2 : e.getValue()) out.println(s2);
        }
        out.close();
        println("DONE  functions=" + hits.size());
    }
}
