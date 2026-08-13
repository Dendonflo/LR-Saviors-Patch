import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

// Cluster B of the 2026-08-13 spike capture is 16 IDENTICAL stacks:
//   MSVCR100+10A3B..10A62 (a tight CRT loop) <- FUN_00b46c20+0x1A3
//   <- FUN_00b47ac0+0x19 <- FUN_00a01a00+0x14A <- the main loop.
// FUN_00b46c20 walks a block list, aligns and merges, memcpys, and moves
// entries between free lists under a CRITICAL_SECTION - a heap compactor.
// FUN_00a01a00 cannot be timed (SEH prologue; return-hijack hooks crashed on
// it and it is deliberately disabled), so to measure the compactor the hook
// has to go on FUN_00b46c20 or its tiny vtable-driven caller FUN_00b47ac0.
//
// The three things that decide whether that is safe, all learned the hard way
// on this project:
//   1. SEH prologue (PUSH -1 / PUSH handler / MOV EAX,FS:[0])? -> return
//      hijacking is off the table.
//   2. Enough prologue bytes for a 5-byte JMP rel32 landing on an instruction
//      boundary, with cumulative offsets so patchLen can be set exactly.
//   3. Any inbound reference INTO the prologue (a jump target inside the
//      stolen bytes), which would land mid-patch.
public class DefragHook extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/defrag_hook.txt", "UTF-8");

        String[] fns = { "00b46c20", "00b47ac0", "00b47af0", "00b45f80" };
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
                out.printf("   %s  %-24s %-34s ends at +%d%n",
                           ins.getAddress(), hex.toString(), txt, cum);
                ins = ins.getNext();
            }
            out.println("   SEH prologue: " + (seh ? "YES - do NOT hijack the return"
                                                   : "no FS: access in the first 12 insns"));

            // Inbound references landing INSIDE the first 8 bytes.
            out.println("   -- refs into the first 8 bytes (must be none) --");
            int bad = 0;
            for (int off = 1; off < 8; off++) {
                Address probe = a.add(off);
                for (Reference r : getReferencesTo(probe)) {
                    out.printf("      +%d  from %s  (%s)%n", off, r.getFromAddress(),
                               r.getReferenceType());
                    bad++;
                }
            }
            if (bad == 0) out.println("      (none - safe to steal the prologue)");

            if (f != null) {
                out.println("   -- callers --");
                int n = 0;
                for (Reference r : getReferencesTo(f.getEntryPoint())) {
                    if (!r.getReferenceType().isCall()) continue;
                    Function c = getFunctionContaining(r.getFromAddress());
                    out.printf("      %s  in %s%n", r.getFromAddress(),
                               c != null ? c.getName() : "(none)");
                    if (++n >= 10) break;
                }
                if (n == 0) out.println("      (none - indirect/vtable target)");
            }
            out.println();
        }
        out.close();
        println("wrote defrag_hook.txt");
    }
}
