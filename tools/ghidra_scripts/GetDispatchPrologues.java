import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;

import java.io.PrintWriter;

public class GetDispatchPrologues extends GhidraScript {
    static final String[] TARGETS = {"00ac3040", "00a01a00", "00a015b0"};

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/dispatch_prologues.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        Memory mem = currentProgram.getMemory();

        for (String addrStr : TARGETS) {
            Address addr = currentProgram.getAddressFactory().getAddress(addrStr);
            out.printf("=== %s ===%n", addrStr);
            byte[] buf = new byte[16];
            mem.getBytes(addr, buf);
            StringBuilder hex = new StringBuilder();
            for (byte b : buf) hex.append(String.format("%02X ", b));
            out.println("Raw bytes (16): " + hex);

            Address cur = addr;
            int total = 0;
            while (total < 12) {
                Instruction insn = currentProgram.getListing().getInstructionAt(cur);
                if (insn == null) break;
                out.printf("  %s: %s  (len=%d)%n", cur, insn.toString(), insn.getLength());
                total += insn.getLength();
                cur = cur.add(insn.getLength());
            }
            out.println("Bytes to cover >=5: " + total);

            boolean unsafe = false;
            for (int off = 1; off < total; off++) {
                Address mid = addr.add(off);
                var refs = getReferencesTo(mid);
                if (refs.length > 0) {
                    out.printf("  UNSAFE: %d ref(s) at +%d%n", refs.length, off);
                    unsafe = true;
                }
            }
            if (!unsafe) out.println("  OK: no inbound refs into prologue interior.");
            out.println();
        }

        out.close();
        println("DONE");
    }
}
