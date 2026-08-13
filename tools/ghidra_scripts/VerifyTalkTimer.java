import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

// Standing rule: confirm every decompiler-derived claim against the real
// instructions. The decompiler printed "timer = timer - 0.05" in
// FieldTalkManager case 10, but the constant-pool sweep did not list
// FUN_005de2c0 as a reader of 0.05f. One of the two is wrong.
//
// Dump every float-constant reference inside FUN_005de2c0 with its value, plus
// the raw disassembly of the case-10 block.
public class VerifyTalkTimer extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/verify_talk_timer.txt","UTF-8");
        Address ep = currentProgram.getAddressFactory().getAddress("005de2c0");
        Function f = getFunctionContaining(ep);
        out.println("function " + f.getName() + " @ " + f.getEntryPoint()
                    + "  body=" + f.getBody());
        AddressSetView body = f.getBody();
        Listing lst = currentProgram.getListing();

        out.println("\n######## float-constant reads inside the function ########");
        InstructionIterator it = lst.getInstructions(body, true);
        while (it.hasNext()) {
            Instruction i = it.next();
            for (Reference r : i.getReferencesFrom()) {
                if (!r.getReferenceType().isData()) continue;
                Address t = r.getToAddress();
                if (!currentProgram.getMemory().contains(t)) continue;
                try {
                    int raw = currentProgram.getMemory().getInt(t);
                    float v = Float.intBitsToFloat(raw);
                    if (Float.isNaN(v) || Float.isInfinite(v)) continue;
                    if (Math.abs(v) > 1000.0f || v == 0.0f) continue;
                    out.println("  " + i.getAddress() + "  " + i
                                + "   ; [" + t + "] = " + v + "f");
                } catch (Exception ex) {}
            }
        }

        // The case-10 arm sets state 0xc via  AND 0xffccffff / OR 0xc0000.
        // Find those instructions and dump a window around each.
        out.println("\n######## blocks that write state 0xc (0xc0000) ########");
        it = lst.getInstructions(body, true);
        while (it.hasNext()) {
            Instruction i = it.next();
            String s = i.toString();
            if (!s.contains("c0000") && !s.contains("C0000")) continue;
            out.println("---- around " + i.getAddress() + " ----");
            Instruction p = i;
            for (int k = 0; k < 14 && p != null; k++) p = p.getPrevious();
            for (int k = 0; k < 20 && p != null; k++) {
                StringBuilder sb = new StringBuilder("    " + p.getAddress() + "  " + p);
                for (Reference r : p.getReferencesFrom()) {
                    if (!r.getReferenceType().isData()) continue;
                    try {
                        int raw = currentProgram.getMemory().getInt(r.getToAddress());
                        sb.append("   ; [" + r.getToAddress() + "] = "
                                  + Float.intBitsToFloat(raw) + "f");
                    } catch (Exception ex) {}
                }
                out.println(sb);
                if (p.getAddress().equals(i.getAddress())) sb.append("   <<<<");
                p = p.getNext();
            }
            out.println();
        }
        out.close();
        println("DONE");
    }
}
