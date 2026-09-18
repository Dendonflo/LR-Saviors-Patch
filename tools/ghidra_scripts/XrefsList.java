import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.*;
import java.util.*;

// Generic: for each address in _xref_request.txt print every reference to it
// (from address, containing function, ref type, and the instruction text).
public class XrefsList extends GhidraScript {
    @Override
    public void run() throws Exception {
        String dir = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/";
        PrintWriter out = new PrintWriter(dir + "_xref_result.txt", "UTF-8");
        try (BufferedReader r = new BufferedReader(new FileReader(dir + "_xref_request.txt"))) {
            String s;
            while ((s = r.readLine()) != null) {
                s = s.trim(); if (s.isEmpty()) continue;
                Address addr = currentProgram.getAddressFactory().getAddress(s);
                out.println("=== refs to " + s + " ===");
                int n = 0;
                for (Reference ref : getReferencesTo(addr)) {
                    Function cf = getFunctionContaining(ref.getFromAddress());
                    Instruction ins = currentProgram.getListing().getInstructionAt(ref.getFromAddress());
                    out.printf("  %s  %-10s  in %-16s  %s%n", ref.getFromAddress(), ref.getReferenceType(),
                        cf != null ? cf.getName() : "-", ins != null ? ins.toString() : "");
                    if (++n > 200) { out.println("  ..."); break; }
                }
                out.println();
            }
        }
        out.close(); println("DONE");
    }
}
