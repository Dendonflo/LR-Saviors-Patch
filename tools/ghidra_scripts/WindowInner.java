import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// The not-found behaviour of the two unchecked wait predicates decides whether
// sfShowWindowWithKeyWait can park forever:
//   FUN_00796b60 - behind Window.isWaitingDecideOrCancel (loop 2, by name)
//   FUN_00795d50 - behind Window.isWindowOpening (loop 1, by handle)
// Known-safe reference: FUN_00795df0 (isWindowClosing by id) returns false
// when the window is absent, so its loop always terminates. If either of these
// returns TRUE on a missing window, the script hangs and every statement after
// the message box - including the plant activation - never runs.
public class WindowInner extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/window_inner.txt","UTF-8");
        DecompInterface d = new DecompInterface();
        DecompileOptions o = new DecompileOptions();
        o.setMaxPayloadMBytes(128);
        d.setOptions(o);
        d.openProgram(currentProgram);
        String[][] t = {
            {"00796b60", "isWaitingDecideOrCancel impl (loop 2)"},
            {"00795d50", "isWindowOpening impl (loop 1)"},
            {"00795df0", "isWindowClosing impl (loop 3, known-safe)"},
            {"00795cd0", "window lookup by id"},
        };
        for (String[] x : t) {
            Address a = currentProgram.getAddressFactory().getAddress(x[0]);
            Function f = getFunctionContaining(a);
            if (f == null) { out.println("no function at " + x[0]); continue; }
            out.println("======== " + x[1] + "  " + f.getName()
                        + " @ " + f.getEntryPoint()
                        + " size=" + f.getBody().getNumAddresses() + " ========");
            DecompileResults dr = d.decompileFunction(f, 600, new ConsoleTaskMonitor());
            out.println(dr != null && dr.decompileCompleted()
                        ? dr.getDecompiledFunction().getC() : "FAILED");
            out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
