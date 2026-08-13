import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

// Two new >=40ms signatures caught during a Yuusnaan run, never seen in any
// prior test route:
//   1) EIP=AMDXN32.DLL+833BD / ntdll.dll+50ED8, ebp chain runs through
//      msvcrt.dll+488A9 -> d3dx9_43.dll+EACA7 -> 00A93E23 -> 00A9565B ->
//      00D72825 -> 00C5C011 -> 00C609E3 -> 00C41629 -> 00C42104 ->
//      00C2E539 -> 00C2EBE4. reads=188/15199KB, allocs up to 8104 in the
//      same window - a much bigger I/O/alloc burst than anything else
//      caught this session, and the ONLY signature this whole project has
//      seen that goes through d3dx9_43.dll rather than the game's own
//      native BgLoader/streaming pipeline.
//   2) EIP=009DE0C8/009DE098, recurring 3x with an identical chain:
//      009DF3E7 -> 009DFF8D -> 009A2805 -> 009A291E -> 009A29B4 ->
//      00AB7824 -> 00AB79E7 -> 00A01B4A -> ... (the last three are the
//      already-known traversal-trigger family from
//      DecompileTraversalTrigger.java) - 009DE0C8 itself is an
//      undecompiled child of that chain.
public class DecompileYuusnaanTrigger extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/yuusnaan_trigger.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        String[] addrs = {
            // d3dx9_43.dll caller chain (game-module addresses only, the
            // DLL frames themselves aren't in this program)
            "00A93E23", "00A9565B", "00D72825", "00C5C011", "00C609E3",
            "00C41629", "00C42104", "00C2E539", "00C2EBE4",
            // 009DE0C8/009DE098 chain
            "009DE0C8", "009DE098", "009DF3E7", "009DFF8D", "009A2805",
            "009A291E", "009A29B4",
            // event 3 (00AA8595) and event 17 (00A5F4F6) own EIPs, quick look
            "00AA8595", "00A5F4F6"
        };

        Set<String> done = new LinkedHashSet<>();

        for (String a : addrs) {
            Address addr;
            try {
                addr = currentProgram.getAddressFactory().getAddress(a);
            } catch (Exception e) {
                out.println("=== " + a + " -> invalid address ===\n");
                continue;
            }
            Function f = getFunctionContaining(addr);
            if (f == null) {
                out.println("=== " + a + " -> NO FUNCTION (data/unmapped) ===\n");
                continue;
            }
            String key = f.getEntryPoint().toString();
            out.println("### scan addr " + a + " is inside " + f.getName() + " @ " + key
                        + " (offset +0x" + Long.toHexString(addr.getOffset() - f.getEntryPoint().getOffset()) + ")");
            if (!done.add(key)) {
                out.println("    (already decompiled above)\n");
                continue;
            }
            out.println("=== " + f.getName() + " @ " + key + " size=" + f.getBody().getNumAddresses() + " ===");
            DecompileResults res = decomp.decompileFunction(f, 90, new ConsoleTaskMonitor());
            if (res != null && res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("  decompile failed");
            }
            out.println();
            out.flush();
        }

        decomp.dispose();
        out.close();
        println("DONE");
    }
}
