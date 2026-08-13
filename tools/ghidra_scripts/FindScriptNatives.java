import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Every script layer is excluded (all timing is ms or condition-polling), so
// the 60 FPS trigger races live in the natives the scripts call. The class
// loader resolves natives BY NAME ("can't find native method [%s/%s]"), so
// the native names must exist as strings in the binary. Find the tables that
// bind them - that gives the C++ implementations of sfActionTalk, the talk /
// window completion checks and the callback dispatcher.
public class FindScriptNatives extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/script_natives.txt","UTF-8");
        Listing listing = currentProgram.getListing();

        // names seen in the decompiled scripts, plus the generic api* prefix
        String[] want = { "isWindowClosing", "isWindowOpening", "isWaitingDecideOrCancel",
                          "showMessageWindow", "cancelTalk", "isPlayEnd", "getCurrentState",
                          "isEndJumpTime", "endPuzzleMode", "isFading" };
        Map<String, List<Address>> found = new LinkedHashMap<>();
        for (Data d : listing.getDefinedData(true)) {
            if (!d.hasStringValue()) continue;
            Object v = d.getValue(); if (v == null) continue;
            String s = v.toString();
            for (String w : want)
                if (s.equals(w) || s.endsWith("/" + w))
                    found.computeIfAbsent(w, k -> new ArrayList<>()).add(d.getAddress());
        }
        out.println("======== native-name strings located ========");
        for (Map.Entry<String, List<Address>> e : found.entrySet())
            for (Address a : e.getValue()) out.println("  " + e.getKey() + " @ " + a);
        out.println();

        // Whoever references these strings is the binding table / registrar.
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        Set<String> done = new LinkedHashSet<>();
        out.println("======== functions referencing them ========");
        for (Map.Entry<String, List<Address>> e : found.entrySet()) {
            for (Address a : e.getValue()) {
                for (Reference r : getReferencesTo(a)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f == null) continue;
                    String k = f.getEntryPoint().toString();
                    out.println("  " + e.getKey() + " <- " + f.getName() + " @ " + k
                                + " size=" + f.getBody().getNumAddresses());
                    if (done.add(k) && f.getBody().getNumAddresses() < 6000) {
                        DecompileResults dr = d.decompileFunction(f, 90, new ConsoleTaskMonitor());
                        out.println(dr != null && dr.decompileCompleted()
                                    ? dr.getDecompiledFunction().getC() : "   decompile failed");
                    }
                }
            }
            out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
