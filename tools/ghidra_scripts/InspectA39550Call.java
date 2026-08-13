import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.PrintWriter;

// Verifies (rather than trusts the decompiler's rendering of) the call
// "FUN_00aacf10(1)" inside FUN_00a39550 by dumping the raw disassembly
// around the call site. Also inspects the data reference at 0x02110af0
// (the only "caller" Ghidra found for FUN_00a39550 -- a data xref, not a
// CALL, meaning FUN_00a39550's address is likely stored in a function
// pointer table / vtable rather than called directly from code).
public class InspectA39550Call extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/a39550_call_inspection.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        Function func = getFunctionAt(currentProgram.getAddressFactory().getAddress("00a39550"));
        out.println("=== Raw disassembly of FUN_00a39550 ===");
        if (func != null) {
            InstructionIterator it = currentProgram.getListing().getInstructions(func.getBody(), true);
            while (it.hasNext()) {
                Instruction insn = it.next();
                out.printf("%s: %s%n", insn.getAddress(), insn.toString());
            }
        } else {
            out.println("FUNCTION NOT FOUND");
        }

        out.println();
        out.println("=== Data / references at 0x02110af0 ===");
        Address dataAddr = currentProgram.getAddressFactory().getAddress("02110af0");
        Data data = getDataAt(dataAddr);
        out.println("Data at addr: " + (data != null ? data.toString() : "none defined"));
        out.println("Bytes around 0x02110af0 (32 bytes before/after):");
        try {
            Address start = dataAddr.subtract(32);
            byte[] buf = new byte[80];
            currentProgram.getMemory().getBytes(start, buf);
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < buf.length; i++) {
                if (i % 4 == 0) sb.append(String.format("%n%s: ", start.add(i)));
                sb.append(String.format("%02x ", buf[i]));
            }
            out.println(sb.toString());
        } catch (Exception e) {
            out.println("Could not read bytes: " + e.getMessage());
        }

        out.println();
        out.println("=== What else references 0x02110af0 (and nearby +/-0x20) ===");
        for (long off = -0x20; off <= 0x20; off += 4) {
            Address a = dataAddr.add(off);
            ReferenceIterator refs = getReferencesTo(a).length > 0 ? null : null;
        }
        Reference[] refsToTarget = getReferencesTo(dataAddr);
        for (Reference r : refsToTarget) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.println("  ref from " + r.getFromAddress() + " (" + (f != null ? f.getName() : "no func") + ") type=" + r.getReferenceType());
        }

        out.close();
        println("DONE");
    }
}
