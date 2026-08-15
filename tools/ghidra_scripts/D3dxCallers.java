import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.*;

// The IAT walk found the true engine-side callers behind the thunks:
//   FUN_00aa2710 -> D3DXCreateTextureFromFileInMemoryEx (call at 00aa2771;
//                   its return address 00AA2776 is the recurring frame in
//                   every d3dx watchdog capture - this is the hot one)
//   FUN_00aa3250 -> D3DXLoadSurfaceFromMemory (00aa3467)
//                   and D3DXLoadVolumeFromMemory (00aa37e3)
// Decompile those, their callers, and the format-decision helpers so the
// Format/Filter/MipFilter arguments and where they come from are readable.
public class D3dxCallers extends GhidraScript {
    PrintWriter out;
    DecompInterface dec;
    Set<String> done = new HashSet<>();

    void dump(String addr, String why, int depth) throws Exception {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        Function f = getFunctionContaining(a);
        if (f == null) { out.println("// no function at " + addr); return; }
        if (!done.add(f.getEntryPoint().toString())) return;
        out.println("################ " + f.getName() + " @ " + f.getEntryPoint()
                    + "  (" + f.getBody().getNumAddresses() + " bytes)  [" + why + "] ################");
        List<String> callers = new ArrayList<>();
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            Function c = getFunctionContaining(r.getFromAddress());
            out.println("// caller: " + r.getFromAddress() + " " + r.getReferenceType()
                        + " in " + (c != null ? c.getName() : "?"));
            if (c != null) callers.add(c.getEntryPoint().toString());
        }
        DecompileResults res = dec.decompileFunction(f, 120, monitor);
        out.println(res != null && res.decompileCompleted()
                    ? res.getDecompiledFunction().getC() : "  DECOMPILE FAILED");
        out.println();
        if (depth > 0)
            for (String c : callers) dump("0x" + c, "caller of " + f.getName(), depth - 1);
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/d3dx_callers.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        dump("0x00aa2710", "calls CreateTextureFromFileInMemoryEx (HOT)", 1);
        dump("0x00aa3250", "calls LoadSurface/LoadVolumeFromMemory", 1);
        dec.dispose();
        out.close();
        println("done -> d3dx_callers.txt");
    }
}
