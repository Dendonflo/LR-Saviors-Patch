import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// The frametime square wave is ENTIRELY inside FUN_00ac3040: per-frame capture
// gives r=+0.993 against frame time, 1148us on fast frames vs 7480us on slow
// (6.5x), and the swing accounts for the whole 6.2ms frametime gap. Everything
// else measured - shadow pass, allocations, file reads, WFSO call count - is
// flat.
//
// It is not the sleep-spin: the framerate is unlocked, so ticksPerFrame is 1ms
// against 11ms frames and the deadline is always already past.
//
// That leaves the three helpers it calls before any limiter logic, plus the
// predicate that selects the branch. Looking for a frame counter - something
// mod 9, or a rotating index over 9 slots - and for whichever one does heavy
// work on a subset of frames.
public class PacerHelpers2 extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/pacer_helpers2.txt","UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();

        String[] fns = { "00a96110", "00a96f20", "00a933a0", "00a96090" };
        for (String s : fns) {
            Address a = currentProgram.getAddressFactory().getAddress(s);
            Function f = getFunctionContaining(a);
            out.println("################ FUN_" + s + " ################");
            if (f == null) { out.println("  no function"); continue; }
            out.println("size=" + f.getBody().getNumAddresses() + " bytes");
            DecompileResults r = dec.decompileFunction(f, 120, new ConsoleTaskMonitor());
            out.println(r != null && r.getDecompiledFunction() != null
                        ? r.getDecompiledFunction().getC() : "(decompile failed)");

            // Any small immediate in here is a candidate for a cycle length.
            out.println("---- small immediates (cycle-length candidates) ----");
            InstructionIterator it = lst.getInstructions(f.getBody(), true);
            while (it.hasNext()) {
                Instruction i = it.next();
                String t = i.toString();
                if (t.startsWith("CMP") || t.startsWith("AND") || t.startsWith("TEST")
                    || t.startsWith("DIV") || t.startsWith("IDIV") || t.startsWith("MOD")) {
                    out.println("    " + i.getAddress() + "  " + t);
                }
            }
            out.println();
        }
        out.close();
        println("wrote pacer_helpers2.txt");
    }
}
