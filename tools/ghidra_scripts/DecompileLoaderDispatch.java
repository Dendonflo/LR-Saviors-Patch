import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.util.task.ConsoleTaskMonitor;

public class DecompileLoaderDispatch extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/loader_dispatch_full.txt";
        java.io.PrintWriter out = new java.io.PrintWriter(outPath, "UTF-8");
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        Address addr = currentProgram.getAddressFactory().getAddress("004b5cb0");
        Function func = getFunctionAt(addr);
        DecompileResults res = decomp.decompileFunction(func, 120, monitor);
        out.println(res != null && res.decompileCompleted() ? res.getDecompiledFunction().getC() : "DECOMPILE FAILED");

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
