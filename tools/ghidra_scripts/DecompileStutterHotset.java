import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

// Takes the hot return addresses harvested from the stutter watchdog's raw
// stack scan, maps each to its containing function, and decompiles the unique
// set. Deduped by function so an address cluster inside one function costs one
// decompile, not five.
public class DecompileStutterHotset extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/stutter_hotset.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        String[] addrs = {
            "00aa2a0d", "00a94886", "00aa2c0b", "00d12967", "00aa3892",
            "00ab79e7", "00aa2fa4", "010ad7bb", "00a01b4a", "00ab9082",
            "00a3a872", "004b762a", "00726677", "00d7db08", "00ab7824",
            "00a47862", "00a94b92", "004b75c0", "00770e38", "0083d6e8",
            "007268e8", "004ade8c", "00d12910", "00ab97c9", "0042d449",
            "00cf1ff5", "00da5899", "00da5000", "00da58e1", "00da6990"
        };

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        Set<String> done = new LinkedHashSet<>();

        for (String a : addrs) {
            Address addr = currentProgram.getAddressFactory().getAddress(a);
            Function func = getFunctionContaining(addr);
            if (func == null) {
                out.println("=== " + a + " -> NO FUNCTION (unmapped/data) ===");
                out.println();
                continue;
            }
            String key = func.getEntryPoint().toString();
            out.println("### callsite " + a + " is inside " + func.getName()
                        + " @ " + key + " (offset +0x"
                        + Long.toHexString(addr.getOffset() - func.getEntryPoint().getOffset()) + ")");
            if (!done.add(key)) {
                out.println("    (already decompiled above)");
                out.println();
                continue;
            }
            out.println("=== " + func.getName() + " @ " + key + " size=" + func.getBody().getNumAddresses() + " ===");
            DecompileResults res = decomp.decompileFunction(func, 90, new ConsoleTaskMonitor());
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
