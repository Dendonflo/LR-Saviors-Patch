import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;
import java.util.LinkedHashSet;

public class DecompileRecurringStack extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/recurring_stack_decompiled.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        String[] addrs = {
            "00A95860", "00A57D78", "00A588D7", "00A8ECAE", "00A642E7",
            "00AA78A6", "00AB774F", "00AB7824", "00AB79E7"
        };

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        LinkedHashSet<String> doneFuncs = new LinkedHashSet<>();
        for (String a : addrs) {
            Address addr = currentProgram.getAddressFactory().getAddress(a);
            Function func = getFunctionContaining(addr);
            out.println("=== capture addr " + a + " ===");
            if (func == null) {
                out.println("  no containing function found");
                out.println();
                continue;
            }
            out.printf("  in function %s @ %s (size %d)%n", func.getName(), func.getEntryPoint(), func.getBody().getNumAddresses());
            String key = func.getEntryPoint().toString();
            if (doneFuncs.contains(key)) {
                out.println("  (already decompiled above - same function as an earlier address)");
                out.println();
                continue;
            }
            doneFuncs.add(key);
            DecompileResults res = decomp.decompileFunction(func, 60, new ConsoleTaskMonitor());
            if (res != null && res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("  decompile failed: " + (res != null ? res.getErrorMessage() : "null result"));
            }
            out.println();
        }

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
