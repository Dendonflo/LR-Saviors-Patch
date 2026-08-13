import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// The completion checks the scripts poll. Looking for a flag that is true for
// exactly one frame, or state committed a frame after the flag flips - either
// lets a script exit its poll loop early and write quest status too soon.
public class CompletionNatives extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/completion_natives.txt","UTF-8");
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        String[][] t = {
            {"009db870","Window.isWindowClosing_l"},
            {"009db6d0","Window.isWindowOpening_l"},
            {"009dac70","Window.isWaitingDecideOrCancel"},
            {"009c88c0","Movie.isPlayEnd"},
            {"009dd210","Zone.getCurrentState"},
            {"009b8d80","Field.cancelTalk"},
        };
        for (String[] e : t) {
            Function f = getFunctionContaining(currentProgram.getAddressFactory().getAddress(e[0]));
            if (f == null) { out.println("=== " + e[1] + " @ " + e[0] + " : no function ===\n"); continue; }
            out.println("=== " + e[1] + " -> " + f.getName() + " @ " + f.getEntryPoint()
                        + " size=" + f.getBody().getNumAddresses() + " ===");
            DecompileResults r = d.decompileFunction(f, 120, new ConsoleTaskMonitor());
            out.println(r != null && r.decompileCompleted() ? r.getDecompiledFunction().getC() : " failed");
            out.println(); out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
