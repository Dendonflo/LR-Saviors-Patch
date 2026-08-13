import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;

// The leaf of the chain the main thread is sitting in while blocked inside
// AMDXN32.DLL, plus its callers, plus a caller listing so the entry point of
// the whole path can be named.
public class DecompileDriverStallLeaf extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/driver_stall_leaf.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        String[] targets = {"00aa3250", "00aa2ef0", "00a94770", "00a3a840", "00a47850"};

        for (String t : targets) {
            Address addr = currentProgram.getAddressFactory().getAddress(t);
            Function func = getFunctionAt(addr);
            if (func == null) func = getFunctionContaining(addr);
            out.println("=== " + t + " -> " + (func == null ? "NO FUNCTION" : func.getName()) + " ===");
            if (func == null) { out.println(); continue; }

            // who calls this
            out.println("--- callers ---");
            for (Reference r : getReferencesTo(func.getEntryPoint())) {
                Function c = getFunctionContaining(r.getFromAddress());
                if (c != null) out.println("    " + c.getName() + " @ " + r.getFromAddress());
            }

            // what external/indirect calls it makes (looking for the D3D entry)
            out.println("--- calls out ---");
            Instruction ins = getInstructionAt(func.getEntryPoint());
            int guard = 0;
            while (ins != null && func.getBody().contains(ins.getAddress()) && guard++ < 4000) {
                String m = ins.getMnemonicString();
                if (m.equals("CALL")) {
                    out.println("    " + ins.getAddress() + "  " + ins.toString());
                }
                ins = ins.getNext();
            }

            DecompileResults res = decomp.decompileFunction(func, 90, new ConsoleTaskMonitor());
            if (res != null && res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("  decompile failed");
            }
            out.println();
            out.flush();
        }

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
