import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

// Ghidra types FUN_00796b60 (behind Window.isWaitingDecideOrCancel) as void
// while its caller clearly consumes the return value as a loop condition, so
// the decompiler could not resolve the return semantics. Dump the raw
// disassembly and find every write to EAX plus every RET, to establish what
// the function actually returns on its two assert/failure paths.
//
// If the failure paths leave EAX undefined (i.e. holding whatever the assert
// helper returned) and that can be non-zero, then
//     while (Window.isWaitingDecideOrCancel(name)) { White.delay(); White.delay(); }
// never terminates and the whole script parks - the Gysahl planting bug.
public class WaitRetVal extends GhidraScript {
    private void dump(PrintWriter out, String addr, String label) throws Exception {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        Function f = getFunctionContaining(a);
        if (f == null) { out.println("no function at " + addr); return; }
        out.println("################ " + label + "  " + f.getName()
                    + " @ " + f.getEntryPoint()
                    + " size=" + f.getBody().getNumAddresses() + " ################");
        Listing lst = currentProgram.getListing();
        InstructionIterator it = lst.getInstructions(f.getBody(), true);
        while (it.hasNext()) {
            Instruction i = it.next();
            String s = i.toString();
            StringBuilder sb = new StringBuilder("  " + i.getAddress() + "  " + s);
            // annotate call targets so the EAX provenance is readable
            for (Reference r : i.getReferencesFrom()) {
                if (r.getReferenceType().isCall()) {
                    Function t = getFunctionAt(r.getToAddress());
                    if (t != null) sb.append("        ; -> " + t.getName());
                }
            }
            if (s.startsWith("RET") || s.contains("EAX")) sb.append("   <<<");
            out.println(sb);
        }
        out.println();
    }

    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/wait_retval.txt","UTF-8");
        dump(out, "00796b60", "isWaitingDecideOrCancel impl (loop 2) - THE SUSPECT");
        dump(out, "00795df0", "isWindowClosing impl (loop 3) - known-safe reference");
        dump(out, "0076a260", "assert helper - what does it leave in EAX?");
        out.close();
        println("DONE");
    }
}
