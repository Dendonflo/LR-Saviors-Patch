import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

// FUN_00aa2710 wraps D3DXCreateTextureFromFileInMemoryEx and IS the d3dx
// stutter family (all watchdog scans carry its return address 00AA2776;
// none carry the LoadSurfaceFromMemory site). Its prologue is SEH
// (PUSH -1 / PUSH handler / MOV EAX,FS:[0]) so return hijacking is out -
// entry-only. Confirm a 5-byte patch lands on an instruction boundary and
// nothing jumps into the stolen bytes.
public class TexCreateSafety extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/texcreate_safety.txt", "UTF-8");
        for (String s : new String[]{ "00aa2710", "00aa2ef0" }) {
            Address a = currentProgram.getAddressFactory().getAddress(s);
            Function f = getFunctionContaining(a);
            out.println("######## " + s + (f != null ? "  " + f.getName() : "") + " ########");
            Instruction ins = currentProgram.getListing().getInstructionAt(a);
            int cum = 0;
            for (int k = 0; k < 8 && ins != null; k++) {
                StringBuilder hex = new StringBuilder();
                try { for (byte b : ins.getBytes()) hex.append(String.format("%02X ", b)); }
                catch (Exception e) {}
                cum += ins.getLength();
                out.printf("   %s  %-20s %-30s ends at +%d%n",
                           ins.getAddress(), hex, ins.toString(), cum);
                ins = ins.getNext();
            }
            out.println("   -- inbound refs into first 5 bytes --");
            int bad = 0;
            for (int off = 1; off < 5; off++)
                for (Reference r : getReferencesTo(a.add(off))) { out.println("      " + r); bad++; }
            if (bad == 0) out.println("      (none - safe)");
            out.println();
        }
        out.close();
        println("done -> texcreate_safety.txt");
    }
}
