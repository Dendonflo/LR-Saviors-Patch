import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

// Cause 3 (script/class-loader synchronous parsing) fix feasibility. Every
// option on the table - timing hooks, a memoised binder, a fast-path decrypt
// replacement - needs to know which of these functions can take the standard
// paired hook. Same three checks as defrag_hook.txt: SEH prologue (return
// hijack forbidden), 5-byte JMP boundary with cumulative offsets, inbound
// references into the stolen bytes.
//
//   009dff70  class-load entry seen in every Cause-3 stack (top timing point)
//   009debc0  recursive resolver (per-class bind timing)
//   009ddfe0  native-method binder - the memoisation candidate
//   009fcfd0  decrypt caller
//   009fd110  8-byte block decrypt leaf - the fast-path replacement candidate
public class LoaderHookSafety extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/loader_hook_safety.txt", "UTF-8");

        String[] fns = { "009dff70", "009debc0", "009ddfe0", "009fcfd0", "009fd110" };
        for (String s : fns) {
            Address a = currentProgram.getAddressFactory().getAddress(s);
            Function f = getFunctionContaining(a);
            out.println("################ " + s +
                        (f != null ? "  " + f.getName() + "  (" +
                         f.getBody().getNumAddresses() + " bytes)" : "  (no function)") +
                        " ################");
            Instruction ins = currentProgram.getListing().getInstructionAt(a);
            int cum = 0;
            boolean seh = false;
            for (int k = 0; k < 12 && ins != null; k++) {
                StringBuilder hex = new StringBuilder();
                try { for (byte b : ins.getBytes()) hex.append(String.format("%02X ", b)); }
                catch (Exception e) { hex.append("??"); }
                cum += ins.getLength();
                String txt = ins.toString();
                if (txt.contains("FS:") || txt.contains("fs:")) seh = true;
                out.printf("   %s  %-24s %-36s ends at +%d%n",
                           ins.getAddress(), hex.toString(), txt, cum);
                ins = ins.getNext();
            }
            out.println("   SEH prologue: " + (seh ? "YES - no return hijack"
                                                   : "no FS: access in first 12 insns"));
            out.println("   -- refs into first 8 bytes --");
            int bad = 0;
            for (int off = 1; off < 8; off++) {
                for (Reference r : getReferencesTo(a.add(off))) {
                    out.printf("      +%d from %s (%s)%n", off, r.getFromAddress(),
                               r.getReferenceType());
                    bad++;
                }
            }
            if (bad == 0) out.println("      (none)");
            if (f != null) {
                out.println("   -- callers --");
                int n = 0;
                for (Reference r : getReferencesTo(f.getEntryPoint())) {
                    if (!r.getReferenceType().isCall()) continue;
                    Function c = getFunctionContaining(r.getFromAddress());
                    out.printf("      %s in %s%n", r.getFromAddress(),
                               c != null ? c.getName() : "(none)");
                    if (++n >= 8) { out.println("      ..."); break; }
                }
                if (n == 0) out.println("      (indirect/vtable only)");
            }
            out.println();
        }
        out.close();
        println("wrote loader_hook_safety.txt");
    }
}
