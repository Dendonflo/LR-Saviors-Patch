import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// Last unmapped link: what populates local_240 in FUN_00a32a00 before it is
// handed to FUN_00a31da0. That is where the cascade extent is decided.
// Dump the whole function so the producer is visible in context.
public class Local240 extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/local240.txt","UTF-8");
        DecompInterface d = new DecompInterface();
        DecompileOptions o = new DecompileOptions();
        o.setMaxWidth(200);
        d.setOptions(o);
        d.openProgram(currentProgram);
        Function f = getFunctionContaining(currentProgram.getAddressFactory().getAddress("00a32a00"));
        DecompileResults r = d.decompileFunction(f, 240, new ConsoleTaskMonitor());
        out.println(r != null && r.decompileCompleted() ? r.getDecompiledFunction().getC() : "failed");
        d.dispose(); out.close(); println("DONE");
    }
}
