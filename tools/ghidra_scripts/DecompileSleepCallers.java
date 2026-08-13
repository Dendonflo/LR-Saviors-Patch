import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;

// The main thread calls Sleep ~12,000 times per frame, averaging <1us each
// (Sleep(0) spin-wait), burning ~5.2ms/frame. These are the sampled return
// addresses of the hottest call sites. The question each decompile must
// answer: is the loop comparing elapsed time against a target (a frame
// limiter, which could be removed to lift the 60fps cap) or polling a flag
// or counter (a spinlock waiting on other work, where the cap is a symptom
// and removing the wait would be a correctness bug)?
public class DecompileSleepCallers extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/sleep_callers.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        String[] addrs = {"00AC31C8", "00D5F8E8", "00A960E4"};

        for (String a : addrs) {
            Address addr = currentProgram.getAddressFactory().getAddress(a);
            Function f = getFunctionContaining(addr);
            out.println("################ sleep call site " + a + " ################");
            if (f == null) { out.println("  NO FUNCTION"); out.println(); continue; }
            out.println("containing function: " + f.getName() + " @ " + f.getEntryPoint()
                        + " (offset +0x" + Long.toHexString(addr.getOffset() - f.getEntryPoint().getOffset())
                        + ") size=" + f.getBody().getNumAddresses());

            out.println("--- callers of this function ---");
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                Function c = getFunctionContaining(r.getFromAddress());
                if (c != null) out.println("    " + c.getName() + " @ " + r.getFromAddress());
            }

            // Raw instructions around the call site: a spin loop's shape
            // (compare, conditional jump backwards) is often clearer in the
            // listing than in the decompiled C.
            out.println("--- listing around call site ---");
            Address start = addr.subtract(96);
            Instruction ins = getInstructionAt(start);
            if (ins == null) ins = getInstructionAfter(start);
            int guard = 0;
            while (ins != null && guard++ < 60 && ins.getAddress().getOffset() <= addr.getOffset() + 64) {
                String mark = ins.getAddress().equals(addr) ? "  <== returns here" : "";
                out.println("    " + ins.getAddress() + "  " + ins.toString() + mark);
                ins = ins.getNext();
            }

            DecompileResults res = decomp.decompileFunction(f, 120, new ConsoleTaskMonitor());
            out.println("--- decompiled ---");
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
