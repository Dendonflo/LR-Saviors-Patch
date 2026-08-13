import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;

public class DecompilePriority extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/tools/ghidra_output/priority_decompiled.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();
        Address addr = currentProgram.getAddressFactory().getAddress("00726c20");
        Function func = getFunctionContaining(addr);
        DecompileResults res = decomp.decompileFunction(func, 60, monitor);
        out.println(res != null && res.decompileCompleted() ? res.getDecompiledFunction().getC() : "FAILED");
        decomp.dispose();
        out.close();
        println("DONE");
    }
}
