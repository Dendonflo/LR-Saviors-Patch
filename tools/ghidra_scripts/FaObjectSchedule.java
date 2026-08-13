import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// Gysahl Green planting (scr104.efWildHatakeGisar) does:
//     sfSetFaAiSch(plot,             "fsh_fao_hatake3");   // plot -> planted
//     ...
//     sfSetFaAiSch("pm_faoF03_0016", "fsh_fao_yasai1");    // SEED entity appears
// User observation: when the bug hits, the plot correctly becomes inactive but
// the seed never spawns - i.e. the SECOND call has no effect.
//
// Both map to Field.changeFaObjectSchedule. The question: does it fail
// SILENTLY when the target object is not currently popped/loaded? The engine
// exposes Field.isStableCharaPop, which implies pop is asynchronous - and the
// script never calls it.
public class FaObjectSchedule extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/fa_object_schedule.txt","UTF-8");
        DecompInterface d = new DecompInterface();
        DecompileOptions o = new DecompileOptions();
        o.setMaxPayloadMBytes(128);
        d.setOptions(o);
        d.openProgram(currentProgram);

        String[][] targets = {
            {"009c0490", "Field.changeFaObjectSchedule  <-- THE SEED SPAWN"},
            {"009c05e0", "Field.getFaObjectScheduleName"},
            {"009b63d0", "Field.isStableCharaPop"},
        };
        for (String[] t : targets) {
            Address a = currentProgram.getAddressFactory().getAddress(t[0]);
            Function f = getFunctionContaining(a);
            if (f == null) { out.println("no function at " + t[0]); continue; }
            out.println("================ " + t[1] + "\n   " + f.getName()
                        + " @ " + f.getEntryPoint()
                        + " size=" + f.getBody().getNumAddresses() + " ================");
            DecompileResults dr = d.decompileFunction(f, 500, new ConsoleTaskMonitor());
            out.println(dr != null && dr.decompileCompleted()
                        ? dr.getDecompiledFunction().getC() : "FAILED");
            out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
