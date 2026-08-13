import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// Runtime capture proved the plant activation is never reached: the plot write
// (script line 1139) logs, then NO timer registrations and NO plant write.
// The only substantial thing between them is the blocking message box at
// line 1140, com.sfShowWindowWithKeyWait, which is three poll loops:
//
//   while (Window.isWindowOpening(n))              { White.delay(); }
//   while (Window.isWaitingDecideOrCancel(name))   { White.delay(); White.delay(); }
//   Window.hideWindow(n);
//   while (Window.isWindowClosing(name))           { White.delay(); }
//
// If any of these reports "keep waiting" for a window that no longer exists,
// the script parks forever and everything after line 1140 is dead - which is
// exactly the observed outcome. isWindowClosing is already known to return
// false (safe) when the window is missing. Check the other two, and the
// creation/teardown calls, for the not-found path.
public class WindowWaits extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/window_waits.txt","UTF-8");
        DecompInterface d = new DecompInterface();
        DecompileOptions o = new DecompileOptions();
        o.setMaxPayloadMBytes(128);
        d.setOptions(o);
        d.openProgram(currentProgram);
        String[][] t = {
            {"009dac70", "Window.isWaitingDecideOrCancel  <-- loop 2, name-based"},
            {"009db6d0", "Window.isWindowOpening_l        <-- loop 1, handle-based"},
            {"009db870", "Window.isWindowClosing_l        <-- loop 3, known-safe reference"},
            {"009d7c70", "Window.hideWindow"},
        };
        for (String[] x : t) {
            Address a = currentProgram.getAddressFactory().getAddress(x[0]);
            Function f = getFunctionContaining(a);
            if (f == null) { out.println("no function at " + x[0]); continue; }
            out.println("================ " + x[1] + "\n   " + f.getName()
                        + " @ " + f.getEntryPoint()
                        + " size=" + f.getBody().getNumAddresses() + " ================");
            DecompileResults dr = d.decompileFunction(f, 600, new ConsoleTaskMonitor());
            out.println(dr != null && dr.decompileCompleted()
                        ? dr.getDecompiledFunction().getC() : "FAILED");
            out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
