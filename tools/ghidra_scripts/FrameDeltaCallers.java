import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// Decide whether the frame-delta quantisation can be corrected from a hook.
// The delta function reads its RAW timestamp from param_1+0x30/0x34 and keeps
// its QUANTISED previous timestamp at param_1+0x38/0x3c. If the caller owns
// that struct and fills +0x30 with a plain QueryPerformanceCounter before the
// call, then a detour can recompute the true delta from +0x30 and overwrite
// the out-param - no patching of the engine's own arithmetic needed.
public class FrameDeltaCallers extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/frame_delta_callers.txt","UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        String[] fns = { "00ab93f0", "00ab7e90" };
        for (String s : fns) {
            Address a = currentProgram.getAddressFactory().getAddress(s);
            Function f = getFunctionContaining(a);
            out.println("################ " + s + " ################");
            if (f == null) { out.println("no function"); continue; }
            out.println("name=" + f.getName() + " size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 120, new ConsoleTaskMonitor());
            out.println(r != null && r.getDecompiledFunction() != null
                        ? r.getDecompiledFunction().getC() : "(decompile failed)");
        }
        out.close();
        println("wrote frame_delta_callers.txt");
    }
}
