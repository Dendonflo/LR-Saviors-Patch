import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// Two questions in one run.
//
// (1) FUN_0073cfb0 is the FA-object name resolver used by BOTH
//     changeFaObjectSchedule (009C0490) and getFaObjectScheduleName (009C05E0).
//     Everything in those functions is gated on it returning non-zero.
//     Hypothesis under test: it skips or fails on SUSPENDED objects.
//     r_fao_sch.json:68838 gives fsh_fao_yasai0 (the plant at planting time)
//     u3Suspend = 1, while fsh_fao_hatake2 (the plot) has u3Suspend = 0.
//     If the resolver consults a suspend flag, that asymmetry is the bug.
//
// (2) FUN_00420890 is what setRelativeTimerCallback (009C22A0) actually calls
//     to register "YasaiTimer1" etc. Planting registers TWO timers at script
//     lines 1148-1149 immediately BEFORE the sfSetFaAiSch at 1150 that fails.
//     Check whether registration has any side effect on object/FA state, and
//     whether the timer's deadline is frame-denominated or game-clock based.
public class ResolverAndTimer extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/resolver_and_timer.txt","UTF-8");
        DecompInterface d = new DecompInterface();
        DecompileOptions o = new DecompileOptions();
        o.setMaxPayloadMBytes(128);
        d.setOptions(o);
        d.openProgram(currentProgram);

        String[][] t = {
            {"0073cfb0", "FA-object NAME RESOLVER  <-- suspend hypothesis"},
            {"00420890", "timer registration (behind setRelativeTimerCallback)"},
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
