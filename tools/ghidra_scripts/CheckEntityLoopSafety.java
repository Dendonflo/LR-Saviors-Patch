import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.PrintWriter;

public class CheckEntityLoopSafety extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/entity_loop_safety.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        String[] targets = {"00a41570", "00a46c20"};
        for (String t : targets) {
            Address addr = currentProgram.getAddressFactory().getAddress(t);
            Function func = getFunctionAt(addr);
            out.println("=== FUN_" + t + " ===");
            if (func == null) { out.println("  not found"); continue; }
            out.printf("  size=%d%n", func.getBody().getNumAddresses());
            out.println("  first 8 instructions:");
            var it = currentProgram.getListing().getInstructions(func.getBody(), true);
            int n = 0;
            while (it.hasNext() && n < 8) {
                Instruction insn = it.next();
                out.printf("    %s: %s%n", insn.getAddress(), insn.toString());
                n++;
            }
            // Inbound refs into the interior (not just the entry point) - a
            // safety check used throughout this investigation.
            out.println("  inbound refs into body (excluding entry):");
            var it2 = currentProgram.getListing().getInstructions(func.getBody(), true);
            int refCount = 0;
            while (it2.hasNext()) {
                Instruction insn = it2.next();
                if (insn.getAddress().equals(func.getEntryPoint())) continue;
                ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(insn.getAddress());
                while (refs.hasNext()) {
                    Reference r = refs.next();
                    out.printf("    -> %s referenced from %s%n", insn.getAddress(), r.getFromAddress());
                    refCount++;
                    if (refCount > 10) break;
                }
                if (refCount > 10) break;
            }
            if (refCount == 0) out.println("    (none found)");
            out.println();
        }

        out.close();
        println("DONE");
    }
}
