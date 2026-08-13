import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.Map;

public class InspectFragAndCaller extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/frag_and_caller.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        out.println("=== Raw disassembly of FRAG_00acc120 ===");
        Function frag = getFunctionAt(currentProgram.getAddressFactory().getAddress("00acc120"));
        if (frag != null) {
            var it = currentProgram.getListing().getInstructions(frag.getBody(), true);
            while (it.hasNext()) {
                Instruction insn = it.next();
                out.printf("%s: %s%n", insn.getAddress(), insn.toString());
            }
        }

        String[] targets = {"00acc310"};
        for (String addrStr : targets) {
            Address addr = currentProgram.getAddressFactory().getAddress(addrStr);
            Function func = getFunctionContaining(addr);
            out.println();
            if (func == null) { out.printf("=== %s NO FUNCTION ===%n", addrStr); continue; }
            out.printf("=== %s @ %s (size=%d) ===%n", func.getName(), func.getEntryPoint(), func.getBody().getNumAddresses());
            out.println("--- Callers ---");
            Reference[] refs = getReferencesTo(func.getEntryPoint());
            Map<String, Boolean> seen = new LinkedHashMap<>();
            int cnt = 0;
            for (Reference ref : refs) {
                Function callerFunc = getFunctionContaining(ref.getFromAddress());
                String key = callerFunc != null ? callerFunc.getName() + " @ " + callerFunc.getEntryPoint() : "addr " + ref.getFromAddress();
                if (!seen.containsKey(key)) { seen.put(key, true); out.println("    " + key); if (++cnt>=20) break; }
            }
            out.println("--- Decompiled ---");
            DecompileResults res = decomp.decompileFunction(func, 60, monitor);
            out.println(res != null && res.decompileCompleted() ? res.getDecompiledFunction().getC() : "DECOMPILE FAILED");
        }

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
