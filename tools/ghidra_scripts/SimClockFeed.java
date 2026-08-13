import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Does the quantised frame delta feed the clock the SCRIPTS read?
//
// If it does, a zero-delta frame means the script-visible clock does not
// advance that frame - which would be a direct mechanism linking the sim-delta
// defect to the Gysahl frame-rate dependency, instead of the hand-wave that
// "some field code might behave differently".
//
// Known from earlier work: [DAT_0234f130 + 0x54] is the engine's global "now"
// that script deadlines are expressed in, and it is a MILLISECOND clock rather
// than a frame counter.
//
// The frame delta is produced by FUN_00ac33b0 and stored by its caller
// FUN_00ab93f0 into param_1[0xc] (i.e. +0x30 of the frame/sim object).
//
// Strategy: dump every function that WRITES [reg + 0x54] after loading
// 0234f130, and separately decompile the frame-loop chain, so the two can be
// compared by eye for a shared source.
public class SimClockFeed extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/sim_clock_feed.txt","UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();

        Address g = currentProgram.getAddressFactory().getAddress("0234f130");
        out.println("################ refs to 0234f130 (the script-visible clock object) ################");
        Set<Function> touchers = new LinkedHashSet<>();
        for (Reference r : getReferencesTo(g)) {
            Function f = getFunctionContaining(r.getFromAddress());
            Instruction i = lst.getInstructionAt(r.getFromAddress());
            out.println("  " + r.getFromAddress() + "  "
                        + (f == null ? "(no function)" : f.getName())
                        + "   " + (i == null ? "" : i.toString()));
            if (f != null) touchers.add(f);
        }

        // Of those, which ones WRITE something into +0x54 / +0x4c?
        out.println();
        out.println("################ of those, functions containing a store to +0x54 or +0x4c ################");
        List<Function> writers = new ArrayList<>();
        for (Function f : touchers) {
            InstructionIterator ii = lst.getInstructions(f.getBody(), true);
            boolean hit = false;
            while (ii.hasNext()) {
                Instruction i = ii.next();
                String s = i.toString();
                if (!s.startsWith("MOV") && !s.startsWith("ADD")) continue;
                if ((s.contains("+ 0x54]") || s.contains("+ 0x4c]")) && s.indexOf(']') < s.indexOf(',')) {
                    out.println("  " + f.getName() + " @ " + i.getAddress() + " : " + s);
                    hit = true;
                }
            }
            if (hit) writers.add(f);
        }

        out.println();
        out.println("################ decompiled clock writers ################");
        for (Function f : writers) {
            out.println("======== " + f.getName() + " @ " + f.getEntryPoint() + " ========");
            DecompileResults r = dec.decompileFunction(f, 120, new ConsoleTaskMonitor());
            out.println(r != null && r.getDecompiledFunction() != null
                        ? r.getDecompiledFunction().getC() : "(decompile failed)");
        }

        out.close();
        println("wrote sim_clock_feed.txt");
    }
}
