import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// Talk entry state 0 seeds the timer as  rand(50000) * 0.0001 + 5.0  -> [5,10).
// Confirm FUN_009efb70 is actually a bounded RNG; that random range is what
// makes the 60fps failure probabilistic rather than deterministic, which is
// exactly what the in-game reports describe.
public class CheckRng extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/check_rng.txt","UTF-8");
        DecompInterface d = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.setMaxPayloadMBytes(128);
        d.setOptions(opts);
        d.openProgram(currentProgram);
        for (String t : new String[]{ "009efb70", "005dab60" }) {
            Address a = currentProgram.getAddressFactory().getAddress(t);
            Function f = getFunctionContaining(a);
            if (f == null) { out.println("no function at " + t); continue; }
            out.println("================ " + f.getName() + " @ " + f.getEntryPoint()
                        + " size=" + f.getBody().getNumAddresses() + " ================");
            DecompileResults dr = d.decompileFunction(f, 400, new ConsoleTaskMonitor());
            out.println(dr != null && dr.decompileCompleted()
                        ? dr.getDecompiledFunction().getC() : "FAILED");
            out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
