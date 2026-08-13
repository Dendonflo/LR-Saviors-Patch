import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.util.task.ConsoleTaskMonitor;
import ghidra.program.model.listing.Listing;

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

// User asked: what does the game's own D3D9Ex-vs-plain-D3D9 decision look
// like, and does the game reference any Ex-exclusive API with no plain-D3D9
// equivalent (GetGPUThreadPriority/SetGPUThreadPriority/CheckDeviceState -
// these have no non-Ex counterpart at all, unlike CreateDeviceEx/PresentEx
// which just have plain-D3D9 siblings). Decompiling the actual decision
// function is more direct than inferring it from runtime hook behavior.
public class DecompileD3D9ExDecision extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/d3d9ex_decision.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        Listing listing = currentProgram.getListing();

        String[] namesToFind = {
            "Direct3DCreate9Ex", "Direct3DCreate9",
            "GetGPUThreadPriority", "SetGPUThreadPriority", "CheckDeviceState",
            "PresentEx", "CreateDeviceEx", "ComposeRects", "WaitForVBlank",
            "GetDisplayModeEx", "ResetEx"
        };

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        for (String name : namesToFind) {
            out.println("======== searching for string: \"" + name + "\" ========");
            Set<String> foundAt = new LinkedHashSet<>();
            Data d = getFirstStringMatch(listing, name);
            if (d == null) {
                out.println("  NOT FOUND as a defined string in this program.\n");
                continue;
            }
            Address strAddr = d.getAddress();
            out.println("  string data at " + strAddr);
            ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(strAddr);
            boolean any = false;
            while (refs.hasNext()) {
                Reference r = refs.next();
                Address from = r.getFromAddress();
                Function f = getFunctionContaining(from);
                if (f == null) continue;
                any = true;
                String key = f.getEntryPoint().toString();
                out.println("  referenced from " + from + " inside " + f.getName() + " @ " + key);
                if (foundAt.add(key)) {
                    DecompileResults res = decomp.decompileFunction(f, 90, new ConsoleTaskMonitor());
                    if (res != null && res.decompileCompleted()) {
                        out.println("  --- decompile of " + f.getName() + " ---");
                        out.println(res.getDecompiledFunction().getC());
                    } else {
                        out.println("  decompile failed");
                    }
                }
            }
            if (!any) out.println("  string exists but has NO references (never used as an argument).");
            out.println();
            out.flush();
        }

        decomp.dispose();
        out.close();
        println("DONE");
    }

    private Data getFirstStringMatch(Listing listing, String needle) {
        for (Data d : listing.getDefinedData(true)) {
            if (d.hasStringValue()) {
                Object val = d.getValue();
                if (val != null && val.toString().equals(needle)) {
                    return d;
                }
            }
        }
        return null;
    }
}
