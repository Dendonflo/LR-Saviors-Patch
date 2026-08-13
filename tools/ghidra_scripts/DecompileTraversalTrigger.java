import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

// Same shader-chain address family found before (00AB79E7, 00A01B4A,
// 00AB9082...), but this time appearing in scan addresses of stutter
// records spread across a whole run (not clustered at startup), with 27/28
// sharing nearly identical chains - one consistent trigger, not noise.
// Shader COMPILATION was already ruled out (CreateVertexShader/
// CreatePixelShader measured at 65us/call). Something ELSE in this same
// code region is blocking on AMDXN32.DLL. Decompiling every distinct
// function in the chain to find what.
public class DecompileTraversalTrigger extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/traversal_trigger.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        String[] addrs = {
            "0218A77C", "00AB79E7", "00A01B4A", "00D12910", "00DA6990",
            "00D9E8EE", "00AB957C", "00AB9082", "00D12967", "00B04AB6",
            "00AC6142", "00A9619C", "00A6E39F", "00A4FFF6"
        };

        Set<String> done = new LinkedHashSet<>();

        for (String a : addrs) {
            Address addr;
            try {
                addr = currentProgram.getAddressFactory().getAddress(a);
            } catch (Exception e) {
                out.println("=== " + a + " -> invalid address ===\n");
                continue;
            }
            Function f = getFunctionContaining(addr);
            if (f == null) {
                out.println("=== " + a + " -> NO FUNCTION (data/unmapped) ===\n");
                continue;
            }
            String key = f.getEntryPoint().toString();
            out.println("### scan addr " + a + " is inside " + f.getName() + " @ " + key
                        + " (offset +0x" + Long.toHexString(addr.getOffset() - f.getEntryPoint().getOffset()) + ")");
            if (!done.add(key)) {
                out.println("    (already decompiled above)\n");
                continue;
            }
            out.println("=== " + f.getName() + " @ " + key + " size=" + f.getBody().getNumAddresses() + " ===");
            DecompileResults res = decomp.decompileFunction(f, 90, new ConsoleTaskMonitor());
            if (res != null && res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("  decompile failed");
            }
            out.println();
            out.flush();
        }

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
