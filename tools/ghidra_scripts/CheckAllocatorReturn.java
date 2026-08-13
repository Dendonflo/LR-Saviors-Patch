import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

import java.io.PrintWriter;

public class CheckAllocatorReturn extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/allocator_return_disasm.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        Address addr = currentProgram.getAddressFactory().getAddress("00b454a0");
        Function func = getFunctionAt(addr);
        out.printf("Function size: %d bytes%n", func.getBody().getNumAddresses());
        out.println("--- Full disassembly ---");
        var it = currentProgram.getListing().getInstructions(func.getBody(), true);
        while (it.hasNext()) {
            Instruction insn = it.next();
            out.printf("%s: %s%n", insn.getAddress(), insn.toString());
        }

        out.close();
        println("DONE");
    }
}
