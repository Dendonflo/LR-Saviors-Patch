import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;

public class DecompileShaderCacheLookup extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/shader_cache_lookup.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        // FUN_00a391e0 - the "find or create by tag" function called
        // throughout FUN_00a57c70/FUN_00a58630 with string tags like
        // "CDev.Dw.Renderer.Shader" and small integer sizes/ids. This is the
        // most likely place a fixed cache capacity or eviction policy lives.
        String[] targets = {"00a391e0"};
        for (String t : targets) {
            Address addr = currentProgram.getAddressFactory().getAddress(t);
            Function func = getFunctionAt(addr);
            out.println("=== FUN_" + t + " ===");
            if (func == null) { out.println("  not found"); out.println(); continue; }
            out.printf("  size=%d%n", func.getBody().getNumAddresses());
            DecompileResults res = decomp.decompileFunction(func, 60, new ConsoleTaskMonitor());
            if (res != null && res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("  decompile failed");
            }
            out.println();
        }

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
