import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// FUN_005de2c0 (FieldTalkManager.cpp, 10724 bytes) exceeded the size guard in
// TalkTimerMgr.java and was never decompiled. It is the talk-manager update /
// state machine - the direct suspect for "a trigger after interaction that
// doesn't fire properly". Give it a long timeout and no size guard.
public class TalkMgrBig extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/talk_mgr_big.txt","UTF-8");
        String[] targets = { "005de2c0" };
        DecompInterface d = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.setMaxPayloadMBytes(128);
        d.setOptions(opts);
        d.openProgram(currentProgram);
        for (String t : targets) {
            Address a = currentProgram.getAddressFactory().getAddress(t);
            Function f = getFunctionContaining(a);
            if (f == null) { out.println("no function at " + t); continue; }
            out.println("=== " + f.getName() + " @ " + f.getEntryPoint()
                        + " size=" + f.getBody().getNumAddresses() + " ===");
            DecompileResults dr = d.decompileFunction(f, 900, new ConsoleTaskMonitor());
            if (dr != null && dr.decompileCompleted()) {
                out.println(dr.getDecompiledFunction().getC());
            } else {
                out.println("FAILED: " + (dr == null ? "null" : dr.getErrorMessage()));
            }
            out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
