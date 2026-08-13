import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.*;

// Constant-centric sweep. SSE has no float immediates, so every hard-coded
// per-frame step must live in the constant pool and be loaded from memory.
// Find every 4-byte constant whose float value looks like a per-frame step,
// then list who reads it. This catches MOVSS-then-SUBSS, which the
// operand-based scan missed.
//
// Also dump the raw disassembly of the FieldTalkManager case-10 timer so the
// exact instruction form is on record.
public class StepConstScan extends GhidraScript {

    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/step_const_scan.txt","UTF-8");

        // --- part 0: the known-good example, raw ---
        out.println("######## FieldTalkManager case 10 timer, raw disassembly ########");
        Address s = currentProgram.getAddressFactory().getAddress("005e0960");
        Address e = currentProgram.getAddressFactory().getAddress("005e0a00");
        Listing lst = currentProgram.getListing();
        for (Instruction i = lst.getInstructionAt(s);
             i != null && i.getAddress().compareTo(e) < 0;
             i = i.getNext()) {
            StringBuilder sb = new StringBuilder("  " + i.getAddress() + "  " + i);
            for (Reference r : i.getReferencesFrom()) {
                if (!r.getReferenceType().isData()) continue;
                try {
                    int raw = currentProgram.getMemory().getInt(r.getToAddress());
                    sb.append("   ; [" + r.getToAddress() + "] = " + Float.intBitsToFloat(raw)
                              + "f / 0x" + Integer.toHexString(raw));
                } catch (Exception ex) {}
            }
            out.println(sb);
        }
        out.println();

        // --- part 1: per-frame-step constants and their readers ---
        // Values a programmer types for a frame step. Anything else (0.5, 1.0,
        // 2.0, 3.14...) is general math and would drown the signal.
        float[] want = {
            0.05f, 0.1f, 0.15f, 0.2f, 0.25f, 0.02f, 0.04f, 0.01f, 0.03f,
            1.0f/30.0f, 1.0f/60.0f, 1.0f/15.0f, 1.0f/20.0f, 0.0166667f, 0.0333333f
        };
        Set<Integer> bits = new LinkedHashSet<>();
        for (float f : want) bits.add(Float.floatToRawIntBits(f));
        // tolerate the compiler's rounding of 1/30 and 1/60
        Map<Integer,Float> extra = new LinkedHashMap<>();
        for (int d = -2; d <= 2; d++) {
            for (float f : new float[]{1.0f/30.0f, 1.0f/60.0f}) {
                int b = Float.floatToRawIntBits(f) + d;
                extra.put(b, Float.intBitsToFloat(b));
            }
        }
        bits.addAll(extra.keySet());

        // constant -> set of reading functions
        Map<Float,Map<String,List<String>>> byVal = new TreeMap<>();
        int scanned = 0;
        for (MemoryBlock mb : currentProgram.getMemory().getBlocks()) {
            if (!mb.isInitialized() || mb.isExecute()) continue;
            Address a = mb.getStart();
            Address end = mb.getEnd();
            while (a.compareTo(end) < 0) {
                int raw;
                try { raw = currentProgram.getMemory().getInt(a); }
                catch (Exception ex) { break; }
                scanned++;
                if (bits.contains(raw)) {
                    float v = Float.intBitsToFloat(raw);
                    List<Reference> refs = new ArrayList<>();
                    for (Reference r : getReferencesTo(a)) refs.add(r);
                    if (!refs.isEmpty()) {
                        Map<String,List<String>> m =
                            byVal.computeIfAbsent(v, k -> new LinkedHashMap<>());
                        for (Reference r : refs) {
                            Function f = getFunctionContaining(r.getFromAddress());
                            String fk = f == null ? "(none)"
                                       : f.getName() + " @ " + f.getEntryPoint();
                            Instruction ins = lst.getInstructionAt(r.getFromAddress());
                            m.computeIfAbsent(fk, k -> new ArrayList<>())
                             .add("      " + r.getFromAddress() + "  "
                                  + (ins == null ? "?" : ins.toString())
                                  + "   const@" + a);
                        }
                    }
                }
                try { a = a.add(4); } catch (Exception ex) { break; }
            }
        }

        out.println("######## scanned " + scanned + " dwords of data ########\n");
        for (Map.Entry<Float,Map<String,List<String>>> e2 : byVal.entrySet()) {
            int total = 0;
            for (List<String> l : e2.getValue().values()) total += l.size();
            out.println("==== constant " + e2.getKey() + "f  ("
                        + e2.getValue().size() + " functions, " + total + " reads) ====");
            for (Map.Entry<String,List<String>> fe : e2.getValue().entrySet()) {
                out.println("  " + fe.getKey());
                for (String l : fe.getValue()) out.println(l);
            }
            out.println();
        }
        out.close();
        println("DONE  values=" + byVal.size());
    }
}
