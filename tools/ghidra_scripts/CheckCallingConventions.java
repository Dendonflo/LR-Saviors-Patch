import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;

// Disassembles a window around actual CALL sites to FUN_00a2ada0 and
// FUN_00d19a00 to determine true stack-cleanup convention: if the
// instruction right after CALL is "ADD ESP, N" the caller cleans up
// (cdecl-style, safe to wrap with our own CALL); if not, the callee's own
// RET N cleans up (stdcall/thiscall-style) and a CALL-wrapping timing hook
// would corrupt the stack unless we account for exactly N.
public class CheckCallingConventions extends GhidraScript {
    static final String[] TARGETS = {"00a2ada0", "00d19a00"};

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/calling_conventions.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        for (String addrStr : TARGETS) {
            Address funcAddr = currentProgram.getAddressFactory().getAddress(addrStr);
            out.printf("=== Call sites for %s ===%n", addrStr);
            Reference[] refs = getReferencesTo(funcAddr);
            int shown = 0;
            for (Reference ref : refs) {
                if (shown >= 5) break;
                Address callSite = ref.getFromAddress();
                Instruction callInsn = currentProgram.getListing().getInstructionAt(callSite);
                if (callInsn == null || !callInsn.toString().startsWith("CALL")) continue;
                Function caller = getFunctionContaining(callSite);
                out.printf("--- call site %s in %s ---%n", callSite,
                        caller != null ? caller.getName() + " @ " + caller.getEntryPoint() : "?");
                // print 6 instructions before and 4 after
                Instruction cur = callInsn;
                for (int i = 0; i < 8 && cur != null; i++) {
                    cur = cur.getPrevious();
                }
                if (cur == null) cur = currentProgram.getListing().getInstructionAt(callSite);
                for (int i = 0; i < 12 && cur != null; i++) {
                    String marker = cur.getAddress().equals(callSite) ? " <== CALL" : "";
                    out.printf("  %s: %s%s%n", cur.getAddress(), cur.toString(), marker);
                    cur = cur.getNext();
                }
                out.println();
                shown++;
            }
            out.println();
        }

        out.close();
        println("DONE");
    }
}
