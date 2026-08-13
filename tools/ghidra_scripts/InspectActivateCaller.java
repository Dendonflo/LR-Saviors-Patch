import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;

public class InspectActivateCaller extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/activate_caller_inspection.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        out.println("=== Raw disassembly of FUN_00a38ec0 (verify real ECX arg to FUN_00aaf6f0) ===");
        Function f1 = getFunctionAt(currentProgram.getAddressFactory().getAddress("00a38ec0"));
        if (f1 != null) {
            var it = currentProgram.getListing().getInstructions(f1.getBody(), true);
            while (it.hasNext()) {
                Instruction insn = it.next();
                out.printf("%s: %s%n", insn.getAddress(), insn.toString());
            }
        }

        out.println();
        out.println("=== Resolving addr 00acba42 (unresolved caller of FUN_00a38ec0) ===");
        Address callerAddr = currentProgram.getAddressFactory().getAddress("00acba42");
        Function containing = getFunctionContaining(callerAddr);
        out.println("getFunctionContaining: " + (containing != null ? containing.getName() + " @ " + containing.getEntryPoint() : "NONE"));

        out.println("--- raw disasm window 00acb9f0-00acba60 (spans FUN_00acb9f0 and neighborhood) ---");
        Address winStart = currentProgram.getAddressFactory().getAddress("00acb9f0");
        Address winEnd = currentProgram.getAddressFactory().getAddress("00acba60");
        var it2 = currentProgram.getListing().getInstructions(
            currentProgram.getAddressFactory().getAddressSet(winStart, winEnd), true);
        while (it2.hasNext()) {
            var insn = it2.next();
            out.printf("%s: %s%n", insn.getAddress(), insn.toString());
        }

        out.close();
        println("DONE");
    }
}
