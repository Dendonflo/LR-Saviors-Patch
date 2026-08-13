import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// The real implementations behind the VM glue. These are what actually decide
// "is the window still closing / is the movie finished" - the completion flags
// the scripts poll.
public class InnerImpl extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/inner_impl.txt","UTF-8");
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        String[][] t = {
            {"00795df0","isWindowClosing (by id)"},
            {"00795f50","isWindowClosing (by name)"},
            {"00795d50","isWindowOpening (by id)"},
            {"00795e90","isWindowOpening (by name)"},
            {"00796b60","isWaitingDecideOrCancel"},
            {"00727d00","Zone.getCurrentState"},
            {"005892e0","Field.cancelTalk"},
        };
        for (String[] e : t) {
            Function f = getFunctionContaining(currentProgram.getAddressFactory().getAddress(e[0]));
            if (f == null) { out.println("=== " + e[1] + " : none ===\n"); continue; }
            out.println("=== " + e[1] + " -> " + f.getName() + " size=" + f.getBody().getNumAddresses() + " ===");
            DecompileResults r = d.decompileFunction(f, 120, new ConsoleTaskMonitor());
            out.println(r != null && r.decompileCompleted() ? r.getDecompiledFunction().getC() : " failed");
            out.println(); out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
