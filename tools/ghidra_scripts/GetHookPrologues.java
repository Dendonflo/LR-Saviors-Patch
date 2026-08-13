import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;

import java.io.PrintWriter;

// Dumps the exact prologue bytes + first several disassembled instructions of
// our two hook targets (FUN_00aacf10, FUN_00d19a00), and confirms whether any
// other code in the binary jumps/calls into the middle of the first 5-6 bytes
// (which would make a standard 5-byte JMP trampoline hook unsafe there).
public class GetHookPrologues extends GhidraScript {
    static final String[] TARGETS = {"00aacf10", "00d19a00"};

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/hook_prologues.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        Memory mem = currentProgram.getMemory();

        for (String addrStr : TARGETS) {
            Address addr = currentProgram.getAddressFactory().getAddress(addrStr);
            Function func = getFunctionAt(addr);
            out.printf("=== %s @ %s  name=%s  callingConv=%s ===%n", addrStr, addr,
                    func != null ? func.getName() : "?",
                    func != null ? func.getCallingConventionName() : "?");

            byte[] buf = new byte[16];
            mem.getBytes(addr, buf);
            StringBuilder hex = new StringBuilder();
            for (byte b : buf) hex.append(String.format("%02X ", b));
            out.println("Raw bytes (16): " + hex);

            out.println("Disassembly:");
            Address cur = addr;
            int total = 0;
            while (total < 12) {
                Instruction insn = currentProgram.getListing().getInstructionAt(cur);
                if (insn == null) break;
                out.printf("  %s: %s  (len=%d)%n", cur, insn.toString(), insn.getLength());
                total += insn.getLength();
                cur = cur.add(insn.getLength());
            }
            out.println("Bytes needed to cover >=5 for JMP patch: " + total);

            // check for any incoming reference landing strictly inside [addr+1, addr+total-1]
            out.println("Checking for inbound refs into the middle of the prologue...");
            boolean unsafe = false;
            for (int off = 1; off < total; off++) {
                Address mid = addr.add(off);
                var refs = getReferencesTo(mid);
                if (refs.length > 0) {
                    out.printf("  UNSAFE: %d reference(s) land at %s (offset +%d)%n", refs.length, mid, off);
                    unsafe = true;
                }
            }
            if (!unsafe) out.println("  OK: no inbound refs into the prologue interior.");
            out.println();
        }

        out.close();
        println("DONE");
    }
}
