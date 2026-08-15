import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

// The d3dx9 stutter family resolved to D3DXLoadSurfaceFromMemory's internal
// conversion loops, and every watchdog scan for those records carries
// 0059E143 as the top game-code frame. Decompile the function containing it
// (= the engine's surface-upload/conversion site), plus the recurring
// second-tier frames, and list callers so the subsystem is identifiable.
public class D3dxCallsite extends GhidraScript {
    PrintWriter out;

    void show(DecompInterface dec, String addr) throws Exception {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        Function f = getFunctionContaining(a);
        out.println("################ " + addr + " ################");
        if (f == null) { out.println("  (no function)"); return; }
        out.println("// " + f.getName() + " @ " + f.getEntryPoint()
                    + "  (" + f.getBody().getNumAddresses() + " bytes)");
        out.println("// -- callers --");
        int n = 0;
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            Function c = getFunctionContaining(r.getFromAddress());
            out.println("//    " + r.getFromAddress() + " " + r.getReferenceType()
                        + " in " + (c != null ? c.getName() : "?"));
            if (++n >= 20) { out.println("//    ..."); break; }
        }
        DecompileResults res = dec.decompileFunction(f, 90, monitor);
        if (res != null && res.decompileCompleted())
            out.println(res.getDecompiledFunction().getC());
        else
            out.println("  DECOMPILE FAILED");
        out.println();
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/d3dx_callsite.txt", "UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        show(dec, "0x0059e143");   // constant top frame in every d3dx record
        show(dec, "0x00aa2776");   // recurring second frame
        show(dec, "0x007bc5ef");   // recurring lower frame
        dec.dispose();
        out.close();
        println("done -> d3dx_callsite.txt");
    }
}
