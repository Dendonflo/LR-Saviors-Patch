import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

public class DecompShadowQuality extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/shadow_quality.txt","UTF-8");
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        String[] addrs = {"00acac90","00acacc0","00c5c050","00acaea0","00acaee0","00acaf20"};
        for (String a : addrs) {
            Address ad = currentProgram.getAddressFactory().getAddress(a);
            Function f = getFunctionContaining(ad);
            if (f == null) { out.println("=== "+a+" no function ===\n"); continue; }
            out.println("=== "+a+" -> "+f.getName()+" @ "+f.getEntryPoint()
                        +" size="+f.getBody().getNumAddresses()+" ===");
            DecompileResults r = d.decompileFunction(f, 90, new ConsoleTaskMonitor());
            out.println(r!=null&&r.decompileCompleted()? r.getDecompiledFunction().getC():"  failed");
            out.println(); out.flush();
        }
        d.dispose(); out.close(); println("DONE");
    }
}
