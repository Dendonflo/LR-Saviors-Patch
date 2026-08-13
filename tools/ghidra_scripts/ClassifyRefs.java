// The earlier BFS treated every reference (including "address taken" /
// data references used to populate function-pointer tables) the same as an
// actual CALL, which produced a misleading chain leading into one-time init
// code. This script re-examines the same target functions but explicitly
// labels each incoming reference by its RefType (real call vs. data/address
// -of), so we can tell whether load_block/prefetch_map are truly invoked
// from a per-frame path or only have their address taken for a callback
// table (in which case the *real* caller is whatever invokes that callback).

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.RefType;

import java.io.PrintWriter;

public class ClassifyRefs extends GhidraScript {

    static final String[] TARGETS = {
        "00496070", "00494f40", "004b5cb0", "004b68e0", "004b6b20", "004b6cc0", "00496f20"
    };

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/tools/ghidra_output/ref_classification.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        for (String addrStr : TARGETS) {
            Address addr = currentProgram.getAddressFactory().getAddress(addrStr);
            Function func = getFunctionContaining(addr);
            String fname = func != null ? func.getName() + " @ " + func.getEntryPoint() : "UNKNOWN";
            out.printf("=== refs to %s (%s) ===%n", addrStr, fname);

            Reference[] refs = getReferencesTo(func != null ? func.getEntryPoint() : addr);
            for (Reference ref : refs) {
                Address fromAddr = ref.getFromAddress();
                RefType rt = ref.getReferenceType();
                Function callerFunc = getFunctionContaining(fromAddr);
                String callerName = callerFunc != null ? callerFunc.getName() + " @ " + callerFunc.getEntryPoint() : "no function";
                out.printf("  from %s : type=%s isCall=%s isData=%s  in %s%n",
                    fromAddr, rt, rt.isCall(), rt.isData(), callerName);
            }
            out.println();
        }

        out.close();
        println("DONE. Written to " + outPath);
    }
}
