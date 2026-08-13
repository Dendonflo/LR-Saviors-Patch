import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Scaling the screen-buffer descriptors at creation produced a MIXED state:
// scaled targets exist, but stock 1920x1080 ones keep being created mid-run
// and the shadow pass binds those. Hypothesis: a change detector - the
// sibling of FUN_00b014b0, which for the shadow maps compares live surface
// dims against the expected size and rebuilds on mismatch. If the screen set
// has one comparing against screen/2, it un-does our scaling every time.
//
// ShadowMapRes beat the shadow-map detector by WRITING THE VALUE IT COMPARES
// AGAINST. Need to know what the screen-buffer detector compares against to
// do the same - or to hook the detector itself.
//
// Dump: callers of FUN_00b00f10 (the rebuild path runs through them), the
// full FUN_00b014b0 (it may handle both sets), and its neighbours.
public class ScreenBufDetector extends GhidraScript {
    private DecompInterface dec;
    private PrintWriter out;

    private void dump(Function f, String why) {
        out.println("################ " + f.getName() + " @ " + f.getEntryPoint()
                    + "  (" + why + ", size=" + f.getBody().getNumAddresses() + ") ################");
        DecompileResults r = dec.decompileFunction(f, 180, new ConsoleTaskMonitor());
        String c = (r != null && r.getDecompiledFunction() != null)
                   ? r.getDecompiledFunction().getC() : "(failed)";
        if (c.length() > 12000) c = c.substring(0, 12000) + "\n... [truncated]";
        out.println(c);
        out.println();
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/screenbuf_detector.txt","UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        Address alloc = currentProgram.getAddressFactory().getAddress("00b00f10");
        Function fa = getFunctionContaining(alloc);
        out.println("======== callers of FUN_00b00f10 ========");
        Set<Function> callers = new LinkedHashSet<>();
        for (Reference r : getReferencesTo(fa.getEntryPoint())) {
            Function c = getFunctionContaining(r.getFromAddress());
            out.println("  from " + r.getFromAddress()
                        + (c == null ? "  (no function)" : "  in " + c.getName()));
            if (c != null) callers.add(c);
        }
        out.println();
        for (Function c : callers) dump(c, "caller of the screen-buffer allocator");

        Address det = currentProgram.getAddressFactory().getAddress("00b014b0");
        Function fd = getFunctionContaining(det);
        if (fd != null) dump(fd, "known change detector (shadow maps; may cover both)");
        out.close();
        println("wrote screenbuf_detector.txt");
    }
}
