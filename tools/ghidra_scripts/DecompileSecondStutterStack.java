import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;

public class DecompileSecondStutterStack extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/second_stutter_stack_decompiled.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        String[] addrs = {
            "0040ACF0", "00A4598A", "00A47030", "00A41667", "00A385A1", "00AC60ED"
        };

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

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
