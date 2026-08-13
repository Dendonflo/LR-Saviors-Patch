import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Settle the decisive question: is the engine's global "now"
// ([DAT_0234f130 + 0x54]) a millisecond clock or a frame counter?
//
// If it is a frame counter, every deadline expressed in it - including the
// chocobo-girl gate in FUN_005b4fb0 and the talk-manager's "+125" stamps -
// elapses in half the wall-clock time at 60fps as at 30. That is the whole
// 60fps bug class in one fact.
//
// Strategy: find "MOV reg, [0x0234f130]" then a nearby store into [reg+0x54]
// or [reg+0x4c] in the same function - that is the tick.
public class ClockUnits extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/clock_units.txt","UTF-8");
        Address g = currentProgram.getAddressFactory().getAddress("0234f130");
        Listing lst = currentProgram.getListing();
        Set<String> tickFns = new LinkedHashSet<>();

        for (Reference r : getReferencesTo(g)) {
            Instruction load = lst.getInstructionAt(r.getFromAddress());
            if (load == null) continue;
            Function f = getFunctionContaining(r.getFromAddress());
            if (f == null) continue;
            // which register received the pointer?
            String ls = load.toString();
            String reg = null;
            java.util.regex.Matcher m =
                java.util.regex.Pattern.compile("^MOV\\s+(E[A-Z]{2}),").matcher(ls);
            if (m.find()) reg = m.group(1);
            if (reg == null) continue;

            Instruction p = load.getNext();
            for (int k = 0; k < 24 && p != null; k++) {
                String s = p.toString();
                if (s.startsWith("MOV dword ptr [" + reg + " + 0x54]")
                 || s.startsWith("MOV dword ptr [" + reg + " + 0x4c]")
                 || s.startsWith("ADD dword ptr [" + reg + " + 0x54]")
                 || s.startsWith("INC dword ptr [" + reg + " + 0x54]")) {
                    out.println("TICK  " + p.getAddress() + "  " + s
                                + "   in " + f.getName() + " @ " + f.getEntryPoint());
                    tickFns.add(f.getEntryPoint().toString());
                }
                p = p.getNext();
            }
        }

        out.println("\n######## decompiled tick functions ########");
        DecompInterface d = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.setMaxPayloadMBytes(128);
        d.setOptions(opts);
        d.openProgram(currentProgram);
        for (String t : tickFns) {
            Address a = currentProgram.getAddressFactory().getAddress(t);
            Function f = getFunctionContaining(a);
            if (f == null) continue;
            out.println("================ " + f.getName() + " @ " + t
                        + " size=" + f.getBody().getNumAddresses() + " ================");
            if (f.getBody().getNumAddresses() > 20000) { out.println("(too large)"); continue; }
            DecompileResults dr = d.decompileFunction(f, 600, new ConsoleTaskMonitor());
            out.println(dr != null && dr.decompileCompleted()
                        ? dr.getDecompiledFunction().getC() : "FAILED");
            out.flush();
        }
        d.dispose(); out.close();
        println("DONE ticks=" + tickFns.size());
    }
}
