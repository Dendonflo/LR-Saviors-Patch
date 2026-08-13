import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;

// Two independent captures of the Gysahl clobber, in different sessions and
// at different stack addresses, both left the SAME 4 bytes in the script's
// plot-name buffer: A8 E0 7F 01 = 0x017FE0A8. Module base is 0x00E60000 in
// every session, so that is RVA 0x99E0A8 -> Ghidra 0x00D9E0A8.
//
// Whatever lives there is what the reusing code stored in that stack slot.
// Identify it: symbol, containing block, whether it is a vtable/static/string,
// and who references it - the referencing functions are the candidates for
// "what runs during the message-box wait and reuses this stack frame".
public class ClobberValue extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/clobber_value.txt","UTF-8");
        Address a = currentProgram.getAddressFactory().getAddress("00d9e0a8");
        out.println("target 0x00D9E0A8  (runtime 0x017FE0A8, RVA 0x99E0A8)");

        MemoryBlock mb = currentProgram.getMemory().getBlock(a);
        out.println("block: " + (mb == null ? "(none)" : mb.getName()
                    + "  r=" + mb.isRead() + " w=" + mb.isWrite() + " x=" + mb.isExecute()));

        Symbol s = getSymbolAt(a);
        out.println("symbol: " + (s == null ? "(none)" : s.getName()));
        Function f = getFunctionContaining(a);
        out.println("in function: " + (f == null ? "(none)" : f.getName() + " @ " + f.getEntryPoint()));
        Data d = getDataAt(a);
        out.println("data: " + (d == null ? "(none)" : d.getDataType() + " = " + d.getDefaultValueRepresentation()));

        out.println("\nbytes at target:");
        StringBuilder sb = new StringBuilder("  ");
        for (int i = 0; i < 32; i++) {
            try { sb.append(String.format("%02X ", currentProgram.getMemory().getByte(a.add(i)))); }
            catch (Exception e) { break; }
        }
        out.println(sb.toString());

        // as a string, in case it is text
        try {
            StringBuilder t = new StringBuilder();
            for (int i = 0; i < 40; i++) {
                byte b = currentProgram.getMemory().getByte(a.add(i));
                if (b == 0) break;
                t.append((b >= 32 && b < 127) ? (char) b : '.');
            }
            out.println("as string: '" + t + "'");
        } catch (Exception e) {}

        out.println("\nreferences TO this address:");
        int n = 0;
        for (Reference r : getReferencesTo(a)) {
            Function rf = getFunctionContaining(r.getFromAddress());
            out.println("  " + r.getFromAddress() + "  " + r.getReferenceType()
                        + (rf == null ? "" : "   in " + rf.getName() + " @ " + rf.getEntryPoint()));
            if (++n > 40) { out.println("  ..."); break; }
        }
        if (n == 0) out.println("  (none)");

        // Also check the address one pointer-width back/forward: vtables are
        // usually referenced at their base, so the value stored might be
        // base+offset into a table.
        out.println("\nnearby defined symbols:");
        for (int off = -32; off <= 32; off += 4) {
            try {
                Address p = a.add(off);
                Symbol ps = getSymbolAt(p);
                if (ps != null) out.println("  " + p + "  " + ps.getName());
            } catch (Exception e) {}
        }
        out.close();
        println("DONE");
    }
}
