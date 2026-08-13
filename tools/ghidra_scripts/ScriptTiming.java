import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// The decompiled scripts show every wait is a condition poll with a per-frame
// yield:
//     while (!Window.isWindowClosed(n)) { White.delay(); }
// White.delay() is NOT in the 1782-entry native table, so it is an interpreter
// primitive, not a native call. White.sleep(ms) IS a native.
//
// Decompile the whole task/timing native block to establish the model:
// does the script VM step once per RENDERED FRAME (making every one of the
// 340 delay() sites frame-rate dependent), or on a wall clock?
public class ScriptTiming extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/script_timing.txt","UTF-8");
        DecompInterface d = new DecompInterface();
        DecompileOptions o = new DecompileOptions();
        o.setMaxPayloadMBytes(128);
        d.setOptions(o);
        d.openProgram(currentProgram);

        String[][] targets = {
            {"009d4c40", "White.sleep(ms)"},
            {"009d1eb0", "White.poll(taskId)"},
            {"009d1f00", "White.wait(taskId)"},
            {"009d1f60", "White.halt"},
            {"009d1fe0", "White.drawSync"},
            {"009d20b0", "White.getTickCount"},
            {"009d1de0", "White.waitSignal"},
        };
        for (String[] t : targets) {
            Address a = currentProgram.getAddressFactory().getAddress(t[0]);
            Function f = getFunctionContaining(a);
            if (f == null) { out.println("no function at " + t[0]); continue; }
            out.println("================ " + t[1] + "  " + f.getName()
                        + " @ " + f.getEntryPoint()
                        + " size=" + f.getBody().getNumAddresses() + " ================");
            DecompileResults dr = d.decompileFunction(f, 400, new ConsoleTaskMonitor());
            out.println(dr != null && dr.decompileCompleted()
                        ? dr.getDecompiledFunction().getC() : "FAILED");
            out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
