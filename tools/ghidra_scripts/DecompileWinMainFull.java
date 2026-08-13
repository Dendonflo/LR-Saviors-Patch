import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;

public class DecompileWinMainFull extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/winmain_full_decompiled.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        Address addr = currentProgram.getAddressFactory().getAddress("00d12da0");
        Function func = getFunctionContaining(addr);
        out.printf("=== %s @ %s (size=%d) ===%n", func.getName(), func.getEntryPoint(), func.getBody().getNumAddresses());
        DecompileResults res = decomp.decompileFunction(func, 90, monitor);
        out.println(res != null && res.decompileCompleted() ? res.getDecompiledFunction().getC() : "DECOMPILE FAILED: " + (res != null ? res.getErrorMessage() : "null"));

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
