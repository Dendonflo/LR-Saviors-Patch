import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;

public class FindDispatchCallSite extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/dispatch_call_site.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        Address target = currentProgram.getAddressFactory().getAddress("004b5cb0");
        Reference[] refs = getReferencesTo(target);
        for (Reference r : refs) {
            Address callSite = r.getFromAddress();
            var insn = currentProgram.getListing().getInstructionAt(callSite);
            out.printf("call site: %s  insn: %s  refType: %s%n", callSite, insn, r.getReferenceType());
        }

        out.close();
        println("DONE");
    }
}
