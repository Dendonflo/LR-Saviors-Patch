import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.Map;

// Decompiles the hot functions from the event-vs-baseline delta ranking that
// have NOT yet been characterized in this investigation. Of particular
// interest: FUN_004bada0 (sits in the AsyncLoader address neighborhood --
// loader thread entry is FUN_004b68e0, AsyncLoader ctor FUN_004b6b20,
// dispatch switch FUN_004b5cb0), and the 0x00d1xxxx-0x00d5xxxx cluster
// (suspected CRT / heap allocator territory, since WinMain is at 0x00d12da0).
// Truncates each decompile to keep output readable across many functions.
public class DecompileUncharacterized extends GhidraScript {
    static final String[] TARGETS = {
        "004bada0",
        "00d19a00", "00d50940", "00d255c0", "00d19640", "00d46060", "00d4a1c0",
        "00a29d40", "00a92240", "00a29710", "00a46380",
        "00bfb780", "00c177a0",
        "00534690", "00794470", "00aaff00", "009df520", "00845700", "006607e0",
        "00ae3e00"
    };
    static final int MAX_LINES = 60;

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/uncharacterized_hot_decompiled.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        for (String addrStr : TARGETS) {
            Address addr = currentProgram.getAddressFactory().getAddress(addrStr);
            Function func = getFunctionContaining(addr);
            if (func == null) { out.printf("=== %s NO FUNCTION ===%n%n", addrStr); continue; }
            out.printf("=== %s @ %s (size=%d) ===%n", func.getName(), func.getEntryPoint(), func.getBody().getNumAddresses());

            out.println("--- Callers ---");
            Reference[] refs = getReferencesTo(func.getEntryPoint());
            Map<String, Boolean> seen = new LinkedHashMap<>();
            int cnt = 0;
            for (Reference ref : refs) {
                Function cf = getFunctionContaining(ref.getFromAddress());
                String key = cf != null ? cf.getName() + " @ " + cf.getEntryPoint() : "addr " + ref.getFromAddress();
                if (!seen.containsKey(key)) { seen.put(key, true); out.println("    " + key); if (++cnt >= 8) break; }
            }

            out.println("--- Decompiled (truncated) ---");
            DecompileResults res = decomp.decompileFunction(func, 60, monitor);
            if (res != null && res.decompileCompleted()) {
                String[] lines = res.getDecompiledFunction().getC().split("\n");
                int limit = Math.min(lines.length, MAX_LINES);
                for (int i = 0; i < limit; i++) out.println(lines[i]);
                if (lines.length > MAX_LINES) out.printf("... [%d more lines truncated]%n", lines.length - MAX_LINES);
            } else {
                out.println("DECOMPILE FAILED");
            }
            out.println();
        }
        decomp.dispose();
        out.close();
        println("DONE");
    }
}
