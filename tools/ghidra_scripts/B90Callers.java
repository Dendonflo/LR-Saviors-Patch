import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// FUN_00b00b90(w, h, fmt-ish) is the texture-creation helper used by both the
// shadow-map allocator (FUN_00b010c0: formats 9=R32F, 0xf=D24S8) and the
// post-process pyramid allocator (FUN_00b00c00: formats 4 and 7, sizes derived
// from [0x0511558c + 0x18/0x1c] with shifts).
//
// The half-res screen-space shadow buffers (backbuffer/2, R32F + A8R8G8B8)
// must come from ANOTHER caller of the same helper. List every calling
// function, then decompile each one not already known, looking for reads of
// the settings object's screen-size fields (+0x10/+0x14) with a >>1.
public class B90Callers extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/b90_callers.txt","UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        Address b90 = currentProgram.getAddressFactory().getAddress("00b00b90");
        Function fb = getFunctionContaining(b90);
        Map<Function, Integer> callers = new LinkedHashMap<>();
        for (Reference r : getReferencesTo(fb.getEntryPoint())) {
            Function c = getFunctionContaining(r.getFromAddress());
            if (c == null) continue;
            callers.merge(c, 1, Integer::sum);
        }
        out.println("################ callers of FUN_00b00b90 ################");
        for (Map.Entry<Function, Integer> e : callers.entrySet())
            out.println("  " + e.getKey().getName() + " @ " + e.getKey().getEntryPoint()
                        + "   call sites: " + e.getValue());

        Set<String> known = new HashSet<>(Arrays.asList("00b00c00", "00b010c0"));
        out.println();
        for (Function c : callers.keySet()) {
            String entry = c.getEntryPoint().toString();
            if (known.contains(entry)) {
                out.println("======== " + c.getName() + " (already mapped: "
                            + (entry.equals("00b010c0") ? "shadow-map allocator" : "post pyramid")
                            + ") ========");
                continue;
            }
            out.println("======== " + c.getName() + " @ " + entry + " ========");
            DecompileResults r = dec.decompileFunction(c, 180, new ConsoleTaskMonitor());
            String s = (r != null && r.getDecompiledFunction() != null)
                       ? r.getDecompiledFunction().getC() : "(failed)";
            if (s.length() > 9000) s = s.substring(0, 9000) + "\n... [truncated]";
            out.println(s);
        }
        out.close();
        println("wrote b90_callers.txt (" + callers.size() + " calling functions)");
    }
}
