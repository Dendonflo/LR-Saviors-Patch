import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;

// FUN_00a39550's address (0x00a39550) was found sitting inside an apparent
// function-pointer array at 0x02110ad0-0x02110b04 (each slot 4 bytes,
// several distinct function addresses + a repeated "0xa410e0" filler,
// immediately followed by an unrelated "Error Fixed Memory %d" string).
// No direct code xref was found to the exact slot 0x02110af0 (FUN_00a39550's
// slot) -- likely because it's accessed via a computed/scaled-index
// addressing mode (e.g. CALL [base + reg*4]) rather than a fixed operand.
// This checks every slot in the array for ANY incoming reference, and also
// resolves the previously-unresolved "addr 00acc14b" caller of FUN_00aacf10.
public class FindDispatchTableRefs extends GhidraScript {
    static final long[] SLOTS = {
        0x02110ad0L, 0x02110ad4L, 0x02110ad8L, 0x02110adcL,
        0x02110ae0L, 0x02110ae4L, 0x02110ae8L, 0x02110aecL,
        0x02110af0L, 0x02110af4L, 0x02110af8L, 0x02110afcL,
        0x02110b00L, 0x02110b04L
    };

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/dispatch_table_refs.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        out.println("=== Reference check for each slot in the 0x02110ad0-0x02110b04 table ===");
        for (long slot : SLOTS) {
            Address a = currentProgram.getAddressFactory().getAddress(Long.toHexString(slot));
            Reference[] refs = getReferencesTo(a);
            out.printf("slot %s : %d refs%n", a, refs.length);
            for (Reference r : refs) {
                Function f = getFunctionContaining(r.getFromAddress());
                out.println("    from " + r.getFromAddress() + " (" + (f != null ? f.getName() : "no func") + ") type=" + r.getReferenceType());
            }
        }

        out.println();
        out.println("=== Resolving addr 00acc14b (unresolved caller of FUN_00aacf10) ===");
        Address callerAddr = currentProgram.getAddressFactory().getAddress("00acc14b");
        Function containing = getFunctionContaining(callerAddr);
        out.println("getFunctionContaining: " + (containing != null ? containing.getName() + " @ " + containing.getEntryPoint() : "NONE"));
        out.println("Instruction at that address: " + currentProgram.getListing().getInstructionAt(callerAddr));
        out.println("Instruction before: " + currentProgram.getListing().getInstructionBefore(callerAddr));
        // Dump a wider disassembly window around it regardless of function boundaries
        out.println("--- raw disasm window 00acc100-00acc180 ---");
        Address winStart = currentProgram.getAddressFactory().getAddress("00acc100");
        Address winEnd = currentProgram.getAddressFactory().getAddress("00acc180");
        var it = currentProgram.getListing().getInstructions(
            currentProgram.getAddressFactory().getAddressSet(winStart, winEnd), true);
        while (it.hasNext()) {
            var insn = it.next();
            out.printf("%s: %s%n", insn.getAddress(), insn.toString());
        }

        out.close();
        println("DONE");
    }
}
