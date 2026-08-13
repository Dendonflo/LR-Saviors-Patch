import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

import java.io.PrintWriter;

public class CheckCallSiteContext extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/call_site_context.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        Address callSite = currentProgram.getAddressFactory().getAddress("004b6a35");
        Instruction cur = currentProgram.getListing().getInstructionAt(callSite);
        // walk back 6 and forward 10
        for (int i = 0; i < 6 && cur != null; i++) cur = cur.getPrevious();
        for (int i = 0; i < 16 && cur != null; i++) {
            String marker = cur.getAddress().equals(callSite) ? "  <== CALL FUN_004b5cb0" : "";
            out.printf("%s: %s%s%n", cur.getAddress(), cur.toString(), marker);
            cur = cur.getNext();
        }

        out.close();
        println("DONE");
    }
}
