import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// D3D9 interception is a dead end: the engine uploads world*lightViewProj per
// object, so the cascade projection never crosses the API boundary. The extent
// must therefore be computed CPU-side, and patched there like ShadowMapRes
// patches [*(0x0511558c) + 0x24].
//
// Best lead: whatever builds the light projection almost certainly READS the
// shadow map resolution (for texel snapping), so it should sit among the
// functions that touch that same settings object. The 0x00b00c00-0x00b014b0
// cluster is the densest group of those.
public class FindCascadeCalc extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/cascade_calc.txt","UTF-8");
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        String[] addrs = {"00b00c00","00b00f10","00b010c0","00b014b0","00ac4600","00ab93f0"};
        for (String a : addrs) {
            Function f = getFunctionContaining(currentProgram.getAddressFactory().getAddress(a));
            if (f == null) { out.println("=== "+a+" no function ===\n"); continue; }
            out.println("=== "+a+" -> "+f.getName()+" size="+f.getBody().getNumAddresses()+" ===");
            DecompileResults r = d.decompileFunction(f, 90, new ConsoleTaskMonitor());
            out.println(r!=null&&r.decompileCompleted()? r.getDecompiledFunction().getC():"  failed");
            out.println(); out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
