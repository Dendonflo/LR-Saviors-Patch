import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.util.task.ConsoleTaskMonitor;

public class DecompileBgMapLoad extends GhidraScript {
    static final String[] TARGETS = {"004974f0", "00497560", "004012a0"};

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/bgmap_load_decompiled.txt";
        java.io.PrintWriter out = new java.io.PrintWriter(outPath, "UTF-8");
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        for (String addrStr : TARGETS) {
            Address addr = currentProgram.getAddressFactory().getAddress(addrStr);
            Function func = getFunctionAt(addr);
            if (func == null) { out.printf("=== %s NO FUNCTION ===%n%n", addrStr); continue; }
            out.printf("=== %s @ %s (size=%d) ===%n", func.getName(), func.getEntryPoint(), func.getBody().getNumAddresses());
            DecompileResults res = decomp.decompileFunction(func, 90, monitor);
            out.println(res != null && res.decompileCompleted() ? res.getDecompiledFunction().getC() : "DECOMPILE FAILED");
            out.println();
        }

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
