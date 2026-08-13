import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// FUN_0073cfb0 turned out to be an 18-byte wrapper around FUN_0073cef0(x, 1).
// FUN_0073cef0 is the real generic string->ID resolver: it is what
// changeFaObjectSchedule, getFaObjectScheduleName AND the relative-timer
// registration all gate on.
//
// Testing: does it consult a suspend flag / active state, which would explain
// why the plant (fsh_fao_yasai0, u3Suspend=1) fails to resolve while the plot
// (fsh_fao_hatake2, u3Suspend=0) resolves fine one line earlier?
//
// FUN_00420690 is the actual timer-slot writer, for the frame-rate question.
public class RealResolver extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/real_resolver.txt","UTF-8");
        DecompInterface d = new DecompInterface();
        DecompileOptions o = new DecompileOptions();
        o.setMaxPayloadMBytes(128);
        d.setOptions(o);
        d.openProgram(currentProgram);
        String[][] t = {
            {"0073cef0", "REAL string->ID resolver"},
            {"00420690", "relative-timer slot writer"},
        };
        for (String[] x : t) {
            Address a = currentProgram.getAddressFactory().getAddress(x[0]);
            Function f = getFunctionContaining(a);
            if (f == null) { out.println("no function at " + x[0]); continue; }
            out.println("================ " + x[1] + "\n   " + f.getName()
                        + " @ " + f.getEntryPoint()
                        + " size=" + f.getBody().getNumAddresses() + " ================");
            DecompileResults dr = d.decompileFunction(f, 600, new ConsoleTaskMonitor());
            out.println(dr != null && dr.decompileCompleted()
                        ? dr.getDecompiledFunction().getC() : "FAILED");
            out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
