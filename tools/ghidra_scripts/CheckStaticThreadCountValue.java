import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryBlock;

import java.io.PrintWriter;

public class CheckStaticThreadCountValue extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/static_thread_count_value.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        String[] addrs = {"02350664", "02350668", "023508b4", "023508b0"};
        for (String a : addrs) {
            Address addr = currentProgram.getAddressFactory().getAddress(a);
            MemoryBlock block = currentProgram.getMemory().getBlock(addr);
            int val = currentProgram.getMemory().getInt(addr);
            out.printf("DAT_%s (block=%s, initialized=%s): static file value = 0x%x (%d)%n",
                a, block != null ? block.getName() : "?", block != null && block.isInitialized(), val, val);
        }

        out.close();
        println("DONE");
    }
}
