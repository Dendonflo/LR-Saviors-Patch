import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// Descriptor-mode SSAA scales the screen-buffer descriptors at creation, so it
// only takes effect when the engine RE-CREATES those buffers - which today
// means the player changing resolution. To make the scale apply immediately we
// need the engine's own "the screen buffers are stale, rebuild them" trigger.
//
// FUN_00b014b0 is the change detector for all three buffer sets (screen,
// post pyramid, shadow maps). What is needed from it:
//   - WHERE it caches the size the buffers were last built at (writing an
//     impossible value there should force exactly one rebuild, with no window
//     resize and no device Reset - unlike touching the resolution field)
//   - what it compares that against
//   - who calls it, and how often
// FUN_00b00f10 (the allocator itself) and FUN_00b00c00 / FUN_00b010c0 (its
// siblings) are dumped alongside so the field offsets can be cross-read.
public class ScreenBufChangeDetect extends GhidraScript {
    private DecompInterface dec;
    private PrintWriter out;

    private void dump(String addr, String why) {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        Function f = getFunctionContaining(a);
        out.println("################ " + addr + "  (" + why + ") ################");
        if (f == null) { out.println("(no function)"); return; }
        DecompileResults r = dec.decompileFunction(f, 240, new ConsoleTaskMonitor());
        String c = (r != null && r.getDecompiledFunction() != null)
                   ? r.getDecompiledFunction().getC() : "(decompile failed)";
        if (c.length() > 26000) c = c.substring(0, 26000) + "\n... [truncated]";
        out.println(c);
        out.println();
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/screenbuf_changedetect.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        dump("00b014b0", "CHANGE DETECTOR for all three buffer sets");
        dump("00b00f10", "screen-space buffer allocator (the provenance gate)");
        dump("00b00c00", "sibling: post pyramid allocator");
        dump("00b010c0", "sibling: shadow map allocator");

        out.println("======== callers of the change detector 00b014b0 ========");
        Address det = currentProgram.getAddressFactory().getAddress("00b014b0");
        for (ghidra.program.model.symbol.Reference r : getReferencesTo(det)) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.println("   " + r.getFromAddress()
                        + (f == null ? "" : "  in " + f.getName() + " @ " + f.getEntryPoint())
                        + "  " + r.getReferenceType());
        }
        out.println();
        out.println("======== callers of the allocator 00b00f10 ========");
        Address alloc = currentProgram.getAddressFactory().getAddress("00b00f10");
        for (ghidra.program.model.symbol.Reference r : getReferencesTo(alloc)) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.println("   " + r.getFromAddress()
                        + (f == null ? "" : "  in " + f.getName() + " @ " + f.getEntryPoint())
                        + "  " + r.getReferenceType());
        }
        out.close();
        println("wrote screenbuf_changedetect.txt");
    }
}
