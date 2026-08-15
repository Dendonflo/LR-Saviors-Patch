import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

// Wrapper A (FUN_00b47ac0, full compactor pass) and wrapper B
// (FUN_00b47af0, phase-alternating half pass) sit 8 bytes apart in one
// vtable (.rdata 021a48a0 / 021a48a8). Walk backward to the vtable start
// (slot preceded by the COL pointer), resolve the RTTI class name via
// COL+0xC -> TypeDescriptor+8, print every slot with its function, and
// list DATA refs to the vtable base (constructor sites). Raw-dword walk,
// same technique that settled the debug-menu question - Ghidra's RTTI
// analyzer has not run on this project.
public class CompactorVtable extends GhidraScript {

    int rd(Address a) throws Exception {
        return currentProgram.getMemory().getInt(a);
    }

    String cstr(Address a) throws Exception {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 128; i++) {
            byte b = currentProgram.getMemory().getByte(a.add(i));
            if (b == 0) break;
            sb.append((char)(b & 0xff));
        }
        return sb.toString();
    }

    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/compactor_vtable.txt", "UTF-8");
        var af = currentProgram.getAddressFactory();

        // Walk back from wrapper A's slot until the dword no longer points
        // into executable code - that dword should be the COL.
        Address slotA = af.getAddress("0x021a48a0");
        Address cur = slotA;
        for (int i = 0; i < 64; i++) {
            Address prev = cur.add(-4);
            int v = rd(prev);
            Function f = getFunctionContaining(af.getAddress(Integer.toUnsignedString(v, 16)));
            if (f == null) break;
            cur = prev;
        }
        Address vtbl = cur;
        int col = rd(vtbl.add(-4));
        out.println("vtable base: " + vtbl + "   COL: " + String.format("%08x", col));
        try {
            Address colA = af.getAddress(String.format("%08x", col));
            int td = rd(colA.add(0xC));
            Address tdA = af.getAddress(String.format("%08x", td));
            out.println("class: " + cstr(tdA.add(8)));
            out.println("COL offset-in-class: " + rd(colA.add(4)));
        } catch (Exception e) {
            out.println("RTTI resolve failed: " + e);
        }

        // Slots.
        out.println();
        for (int i = 0; i < 24; i++) {
            Address s = vtbl.add(i * 4);
            int v = rd(s);
            Function f = null;
            try { f = getFunctionContaining(af.getAddress(String.format("%08x", v))); }
            catch (Exception e) {}
            String mark = s.equals(af.getAddress("0x021a48a0")) ? "   <== wrapper A (full pass)"
                        : s.equals(af.getAddress("0x021a48a8")) ? "   <== wrapper B (alternating)" : "";
            out.println(String.format("  +0x%02x  %s  %08x  %s%s",
                        i * 4, s, v, f != null ? f.getName() : "(not code)", mark));
            if (f == null && i > 0) break;
        }

        // Constructor sites: DATA refs to the vtable base.
        out.println();
        out.println("-- DATA refs to vtable base (constructors) --");
        for (Reference r : getReferencesTo(vtbl)) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.println("   " + r.getFromAddress() + "  " + r.getReferenceType()
                        + "  in " + (f != null ? f.getName() : "?"));
        }
        out.close();
        println("done -> compactor_vtable.txt");
    }
}
