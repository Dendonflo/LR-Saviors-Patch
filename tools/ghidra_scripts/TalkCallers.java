import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// Two questions, one run:
//  1. Is FieldTalkManager::update (FUN_005de2c0) called once per frame?
//  2. What is [manager+0x10] (local_514)? The state machine stamps deadlines as
//     "local_514 + 125" and later tests "stamp + duration < local_514". If that
//     base is a frame counter, every such deadline halves at 60fps.
// FUN_005dbc70 is the case-0xc teardown - the path that releases a talk slot.
public class TalkCallers extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/talk_callers.txt","UTF-8");
        String[] targets = { "005b5050", "005b4bd0", "0072a720", "005dbc70", "005db9b0", "005dcc40" };
        DecompInterface d = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.setMaxPayloadMBytes(128);
        d.setOptions(opts);
        d.openProgram(currentProgram);
        for (String t : targets) {
            Address a = currentProgram.getAddressFactory().getAddress(t);
            Function f = getFunctionContaining(a);
            if (f == null) { out.println("no function at " + t); continue; }
            out.println("================ " + f.getName() + " @ " + f.getEntryPoint()
                        + " size=" + f.getBody().getNumAddresses() + " ================");
            DecompileResults dr = d.decompileFunction(f, 600, new ConsoleTaskMonitor());
            out.println(dr != null && dr.decompileCompleted()
                        ? dr.getDecompiledFunction().getC()
                        : "FAILED: " + (dr == null ? "null" : dr.getErrorMessage()));
            out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
