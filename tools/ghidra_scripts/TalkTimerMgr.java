import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// The exe embeds source paths in assert calls, so functions can be mapped to
// their original .cpp. FieldTimerScriptManager.cpp and FieldTalkManager.cpp
// are the prime suspects for a frame-denominated callback timer.
public class TalkTimerMgr extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/talk_timer_mgr.txt","UTF-8");
        String[] want = { "FieldTimerScriptManager.cpp", "FieldTalkManager.cpp" };
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        Set<String> done = new LinkedHashSet<>();
        for (Data dat : currentProgram.getListing().getDefinedData(true)) {
            if (!dat.hasStringValue()) continue;
            Object v = dat.getValue(); if (v == null) continue;
            String s = v.toString();
            boolean hit = false;
            for (String w : want) if (s.endsWith(w)) hit = true;
            if (!hit) continue;
            out.println("######## " + s + " @ " + dat.getAddress() + " ########");
            for (Reference r : getReferencesTo(dat.getAddress())) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f == null) continue;
                String k = f.getEntryPoint().toString();
                if (!done.add(k)) continue;
                long sz = f.getBody().getNumAddresses();
                out.println("=== " + f.getName() + " @ " + k + " size=" + sz + " ===");
                if (sz > 8000) { out.println("  (too large)\n"); continue; }
                DecompileResults dr = d.decompileFunction(f, 110, new ConsoleTaskMonitor());
                out.println(dr != null && dr.decompileCompleted() ? dr.getDecompiledFunction().getC() : " failed");
                out.println(); out.flush();
            }
        }
        d.dispose(); out.close(); println("DONE " + done.size());
    }
}
