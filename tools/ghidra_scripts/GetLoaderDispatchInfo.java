import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.Map;

public class GetLoaderDispatchInfo extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/loader_dispatch_info.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        Memory mem = currentProgram.getMemory();
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        var monitor = new ghidra.util.task.ConsoleTaskMonitor();

        for (String addrStr : new String[]{"004b5cb0", "004b68e0"}) {
            Address addr = currentProgram.getAddressFactory().getAddress(addrStr);
            Function func = getFunctionAt(addr);
            out.printf("=== %s @ %s  name=%s  size=%s ===%n", addrStr, addr,
                    func != null ? func.getName() : "?",
                    func != null ? func.getBody().getNumAddresses() : "?");

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

            if (func != null) {
                out.println("--- Callers ---");
                Reference[] refs = getReferencesTo(func.getEntryPoint());
                Map<String, Boolean> seen = new LinkedHashMap<>();
                int cnt = 0;
                for (Reference ref : refs) {
                    Function cf = getFunctionContaining(ref.getFromAddress());
                    String key = cf != null ? cf.getName() + " @ " + cf.getEntryPoint() : "addr " + ref.getFromAddress();
                    if (!seen.containsKey(key)) { seen.put(key, true); out.println("    " + key); if (++cnt >= 10) break; }
                }
                out.println("--- Decompiled signature ---");
                DecompileResults res = decomp.decompileFunction(func, 60, monitor);
                if (res != null && res.decompileCompleted()) {
                    String c = res.getDecompiledFunction().getC();
                    // just the signature line(s) before the first '{'
                    int braceIdx = c.indexOf('{');
                    out.println(braceIdx > 0 ? c.substring(0, braceIdx) : c.substring(0, Math.min(200, c.length())));
                }
            }
            out.println();
        }

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
