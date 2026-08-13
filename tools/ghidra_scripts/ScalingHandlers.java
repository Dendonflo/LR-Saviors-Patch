import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// The three Graphics_Scaling menu handlers, registered in FUN_00acaf60:
//   Advanced -> FUN_00acaea0
//   Standard -> FUN_00acaee0
//   None     -> FUN_00acaf20
// Graphics_Shadowing's handlers turned out to be two-liners writing a single
// settings field, and writing that field WAS the whole ShadowMapRes fix.
// Same play here: read what these three write, and the difference between
// them is the switch for whatever "Scaling" does - the last engine-side
// candidate for the anti-aliasing that survives killing every post shader.
//
// Also dumps whoever READS the written field, since that names the code that
// actually performs the effect.
public class ScalingHandlers extends GhidraScript {
    private DecompInterface dec;
    private PrintWriter out;
    private Set<String> seen = new HashSet<>();

    private Function dump(String addr, String why) {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        Function f = getFunctionContaining(a);
        if (f == null) { out.println("(no function at " + addr + ")"); return null; }
        if (!seen.add(f.getEntryPoint().toString())) return f;
        out.println("################ " + f.getName() + " @ " + f.getEntryPoint()
                    + "   (" + why + ") ################");
        DecompileResults r = dec.decompileFunction(f, 240, new ConsoleTaskMonitor());
        String c = (r != null && r.getDecompiledFunction() != null)
                   ? r.getDecompiledFunction().getC() : "(decompile failed)";
        if (c.length() > 16000) c = c.substring(0, 16000) + "\n... [truncated]";
        out.println(c);
        out.println();
        return f;
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/scaling_handlers.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        dump("00acaea0", "Graphics_Scaling_ADVANCED handler");
        dump("00acaee0", "Graphics_Scaling_STANDARD handler");
        dump("00acaf20", "Graphics_Scaling_NONE handler");

        // For comparison, the equivalent shadowing handlers - the known-good
        // template whose field write became ShadowMapRes.
        dump("00acac90", "Graphics_Shadowing_Advanced (reference template)");
        dump("00acacc0", "Graphics_Shadowing_Standard (reference template)");

        // Whatever globals those handlers touch, find their readers. A
        // setting is only interesting if something consumes it.
        out.println("======== data referenced by the scaling handlers, and its readers ========");
        String[] handlers = { "00acaea0", "00acaee0", "00acaf20" };
        Set<Address> datas = new LinkedHashSet<>();
        for (String h : handlers) {
            Function f = getFunctionContaining(currentProgram.getAddressFactory().getAddress(h));
            if (f == null) continue;
            InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                for (Reference r : ins.getReferencesFrom()) {
                    if (r.getToAddress().isMemoryAddress()
                        && getFunctionContaining(r.getToAddress()) == null) {
                        datas.add(r.getToAddress());
                    }
                }
            }
        }
        for (Address d : datas) {
            out.println("---- data " + d + " ----");
            int n = 0;
            for (Reference r : getReferencesTo(d)) {
                Function rf = getFunctionContaining(r.getFromAddress());
                out.println("    " + r.getFromAddress()
                            + (rf == null ? "" : "  in " + rf.getName()));
                if (++n > 40) { out.println("    ..."); break; }
            }
        }
        out.println();

        // And decompile the readers of those globals (small ones only).
        for (Address d : datas) {
            for (Reference r : getReferencesTo(d)) {
                Function rf = getFunctionContaining(r.getFromAddress());
                if (rf == null) continue;
                if (rf.getBody().getNumAddresses() > 2000) continue;
                dump(rf.getEntryPoint().toString(),
                     "reader of " + d + ", size=" + rf.getBody().getNumAddresses());
            }
        }
        out.close();
        println("wrote scaling_handlers.txt");
    }
}
