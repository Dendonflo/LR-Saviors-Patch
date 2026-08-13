import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// Planting registers two growth callbacks BEFORE the call that can fail:
//     sfSetTimerCallBack("YasaiTimer1");   -> efWildYasaiGrown      (full)
//     sfSetTimerCallBack("YasaiTimerH1");  -> efWildYasaiGrownHalf  (half)
//     sfSetFaAiSch("pm_faoF03_0016", "fsh_fao_yasai1");  <- can silently no-op
//
// Question: does registration depend on the target object existing, and does a
// registered timer survive a reload? If it survives and re-fires, a missing
// plant should eventually be rescued - it is not, so find out why.
public class TimerCallback extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/timer_callback.txt","UTF-8");
        DecompInterface d = new DecompInterface();
        DecompileOptions o = new DecompileOptions();
        o.setMaxPayloadMBytes(128);
        d.setOptions(o);
        d.openProgram(currentProgram);
        String[][] t = {
            {"009c22a0", "Field.setRelativeTimerCallback"},
            {"005e1560", "FieldTimerScriptManager (scheduler)"},
            {"005e2310", "FieldTimerScriptManager"},
            {"005e29a0", "FieldTimerScriptManager"},
        };
        for (String[] x : t) {
            Address a = currentProgram.getAddressFactory().getAddress(x[0]);
            Function f = getFunctionContaining(a);
            if (f == null) { out.println("no function at " + x[0]); continue; }
            out.println("================ " + x[1] + "  " + f.getName()
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
