import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.*;

// D3DX pixel conversion is ~18% of watchdog captures. D3DXLoadSurfaceFromMemory
// only burns CPU when SrcFormat != the destination surface's format; matching
// them turns it into a straight copy. This finds every call site through the
// IAT slots and decompiles the containing functions so the format arguments
// (and where the destination surface comes from) are readable.
//
// IAT slots, from the exe's import table:
//   0x00de9608  D3DXLoadSurfaceFromMemory          (arg5 = SrcFormat)
//   0x00de960c  D3DXLoadVolumeFromMemory           (arg5 = SrcFormat)
//   0x00de9610  D3DXCreateTextureFromFileInMemoryEx(arg8 = Format)
public class D3dxFormats extends GhidraScript {

    PrintWriter out;
    DecompInterface dec;
    Set<String> done = new HashSet<>();

    void dumpFn(Function f, String why) throws Exception {
        if (f == null) return;
        String key = f.getEntryPoint().toString();
        if (!done.add(key)) { out.println("// (already dumped: " + f.getName() + ")\n"); return; }
        out.println("################ " + f.getName() + " @ " + f.getEntryPoint()
                    + "   [" + why + "] ################");
        out.println("// -- callers --");
        int n = 0;
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            Function c = getFunctionContaining(r.getFromAddress());
            out.println("//    " + r.getFromAddress() + "  " + r.getReferenceType()
                        + "  in " + (c != null ? c.getName() : "?"));
            if (++n >= 15) { out.println("//    ..."); break; }
        }
        DecompileResults res = dec.decompileFunction(f, 120, monitor);
        out.println(res != null && res.decompileCompleted()
                    ? res.getDecompiledFunction().getC() : "  DECOMPILE FAILED");
        out.println();
    }

    void viaIat(String iatAddr, String label) throws Exception {
        out.println("=========================================================");
        out.println("=== " + label + "   IAT slot " + iatAddr);
        out.println("=========================================================");
        Address slot = currentProgram.getAddressFactory().getAddress(iatAddr);
        List<Function> sites = new ArrayList<>();
        for (Reference r : getReferencesTo(slot)) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.println("// call site " + r.getFromAddress() + " (" + r.getReferenceType() + ") in "
                        + (f != null ? f.getName() : "?"));
            if (f != null) sites.add(f);
        }
        if (sites.isEmpty()) out.println("// NO direct references - called via a thunk?");
        out.println();
        for (Function f : sites) dumpFn(f, "calls " + label);
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/d3dx_formats.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        viaIat("0x00de9608", "D3DXLoadSurfaceFromMemory");
        viaIat("0x00de9610", "D3DXCreateTextureFromFileInMemoryEx");
        viaIat("0x00de960c", "D3DXLoadVolumeFromMemory");

        // The upload dispatcher the watchdog scans always name, plus its caller.
        out.println("=========================================================");
        out.println("=== surface-upload dispatcher seen in every d3dx capture");
        out.println("=========================================================");
        dumpFn(getFunctionContaining(currentProgram.getAddressFactory().getAddress("0x005a02f0")),
               "caller of the dispatcher");

        dec.dispose();
        out.close();
        println("done -> d3dx_formats.txt");
    }
}
