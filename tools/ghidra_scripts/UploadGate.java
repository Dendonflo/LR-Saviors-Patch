import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

// FUN_00aa3250 is the engine's texture-upload path and it already HAS a
// memcpy fast path. The decompile shows the gate:
//
//   slow = (width  & (width-1))  != 0     // width not a power of two
//       || (height & (height-1)) != 0     // height not a power of two
//       || this[8] != srcFormat;          // format mismatch
//
//   slow == 0 -> row-wise memcpy into the locked surface
//   slow == 1 -> D3DXLoadSurfaceFromMemory  (the 18% stutter family)
//
// To learn WHICH condition fires at runtime we need an entry-only hook
// (arguments are all we want - no return hijack, so SEH is irrelevant).
// This checks the prologue is patchable and dumps the engine-format ->
// D3DFORMAT table at DAT_021886d0 so logged indices are readable.
public class UploadGate extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/upload_gate.txt", "UTF-8");

        String[] fns = { "00aa3250", "00aa3df0", "00aa3dd0", "00aa3010" };
        for (String s : fns) {
            Address a = currentProgram.getAddressFactory().getAddress(s);
            Function f = getFunctionContaining(a);
            out.println("######## " + s + (f != null ? "  " + f.getName() + " (" +
                        f.getBody().getNumAddresses() + " bytes)" : " (none)") + " ########");
            Instruction ins = currentProgram.getListing().getInstructionAt(a);
            int cum = 0; boolean seh = false;
            for (int k = 0; k < 10 && ins != null; k++) {
                StringBuilder hex = new StringBuilder();
                try { for (byte b : ins.getBytes()) hex.append(String.format("%02X ", b)); }
                catch (Exception e) { hex.append("??"); }
                cum += ins.getLength();
                if (ins.toString().contains("FS:")) seh = true;
                out.printf("   %s  %-22s %-32s ends at +%d%n",
                           ins.getAddress(), hex, ins.toString(), cum);
                ins = ins.getNext();
            }
            out.println("   SEH in prologue: " + (seh ? "YES" : "no"));
            out.println("   -- inbound refs into first 8 bytes (must be none) --");
            int bad = 0;
            for (int off = 1; off < 8; off++)
                for (Reference r : getReferencesTo(a.add(off))) { out.println("      " + r); bad++; }
            if (bad == 0) out.println("      (none - safe to steal the prologue)");
            out.println();
        }

        // Engine format index -> D3DFORMAT table used at the call:
        //   *(DWORD *)(&DAT_021886d0 + srcFormat * 4)
        out.println("######## format table at 021886d0 ########");
        Address t = currentProgram.getAddressFactory().getAddress("0x021886d0");
        for (int i = 0; i < 40; i++) {
            int v = currentProgram.getMemory().getInt(t.add(i * 4L));
            String fourcc = "";
            if (v > 0x20202020) {
                fourcc = " '" + (char)(v & 0xff) + (char)((v >> 8) & 0xff)
                              + (char)((v >> 16) & 0xff) + (char)((v >> 24) & 0xff) + "'";
            }
            out.printf("   [%2d] = %d (0x%08x)%s%n", i, v, v, fourcc);
        }
        out.close();
        println("done -> upload_gate.txt");
    }
}
