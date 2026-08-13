import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;

public class CheckAllocatorSafety extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/allocator_safety.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        Memory mem = currentProgram.getMemory();

        Address addr = currentProgram.getAddressFactory().getAddress("00b454a0");
        byte[] buf = new byte[16];
        mem.getBytes(addr, buf);
        StringBuilder hex = new StringBuilder();
        for (byte b : buf) hex.append(String.format("%02X ", b));
        out.println("Raw bytes (16): " + hex);

        Address cur = addr;
        int total = 0;
        while (total < 16) {
            Instruction insn = currentProgram.getListing().getInstructionAt(cur);
            if (insn == null) break;
            out.printf("  %s: %s  (len=%d)%n", cur, insn.toString(), insn.getLength());
            total += insn.getLength();
            cur = cur.add(insn.getLength());
        }

        boolean unsafe = false;
        for (int off = 1; off < total; off++) {
            Address mid = addr.add(off);
            Reference[] refs = getReferencesTo(mid);
            if (refs.length > 0) {
                out.printf("  UNSAFE: %d ref(s) at +%d%n", refs.length, off);
                unsafe = true;
            }
        }
        if (!unsafe) out.println("  OK: no inbound refs into first " + total + " bytes' interior.");

        out.println();
        out.println("--- Callers of FUN_00b454a0 (first 15) ---");
        Reference[] refs = getReferencesTo(addr);
        int cnt = 0;
        for (Reference r : refs) {
            var f = getFunctionContaining(r.getFromAddress());
            out.println("  from " + r.getFromAddress() + " (" + (f != null ? f.getName() + "@" + f.getEntryPoint() : "?") + ")");
            if (++cnt >= 15) break;
        }
        out.printf("Total callers: %d%n", refs.length);

        out.close();
        println("DONE");
    }
}
