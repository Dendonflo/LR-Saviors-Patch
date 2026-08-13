import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;

public class ResolveWrDispatchIntStacks extends GhidraScript {
    static final long ASLR_ADJUST = 0x002B0000L;
    static final String[] RUNTIME_ADDRS = {
        "751b1e", "809082", "807824", "8079e7", "8131d2", "8094be", "a62967"
    };

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/wrdispatchint_resolved.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        for (String a : RUNTIME_ADDRS) {
            long runtime = Long.parseLong(a, 16);
            long ghidraAddr = runtime + ASLR_ADJUST;
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(ghidraAddr);
            Function f = getFunctionContaining(addr);
            out.printf("runtime 0x%s -> ghidra 0x%x -> %s%n", a, ghidraAddr,
                f != null ? (f.getName() + " @ " + f.getEntryPoint() + " (+0x" + Long.toHexString(ghidraAddr - f.getEntryPoint().getOffset()) + ")") : "NO FUNCTION");
        }
        out.close();
        println("DONE");
    }
}
