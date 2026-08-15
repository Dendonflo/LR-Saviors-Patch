import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.mem.MemoryBlock;
import java.io.PrintWriter;

// The 2026-08-15 runs made FUN_00b46c20 (heap compactor) the dominant
// remaining stutter source: ~2 calls/frame at fixed cadence, normally
// 0.2-0.5us, ballooning to 3-10ms per call in NPC-dense areas (Ruffian).
// "Few huge slices at fixed cadence" means the lever is bounding work per
// slice - and the engine looks like it already HAS two modes:
//   FUN_00b47ac0: sets this+0x50=1, calls compactor with [this+0x4c]
//   FUN_00b47af0: checks this+0x51, either FUN_00b472f0([this+0x48])
//                 or compactor via its second call site 00b47b1b
// This script decompiles the whole family plus finds:
//   - the vtable(s) holding b47ac0/b47af0 and neighbouring slots (RTTI name)
//   - what decides which wrapper runs (callers of the vtable slots are
//     indirect, so instead: data refs to the vtables = constructor sites)
//   - every field offset the compactor reads off `this`, to find a
//     work-bound/batch-count if one exists.
public class CompactorFamily extends GhidraScript {

    PrintWriter out;

    void decompile(DecompInterface dec, String addr) throws Exception {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        Function f = getFunctionContaining(a);
        out.println("################ " + addr + " ################");
        if (f == null) { out.println("  (no function here)"); return; }
        out.println("// " + f.getName() + "  " + f.getBody().getNumAddresses() + " bytes");
        DecompileResults r = dec.decompileFunction(f, 120, monitor);
        if (r != null && r.decompileCompleted()) {
            out.println(r.getDecompiledFunction().getC());
        } else {
            out.println("  DECOMPILE FAILED");
        }
        out.println();
    }

    void refsTo(String addr, String label) {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        out.println("-- refs to " + addr + " (" + label + ") --");
        int n = 0;
        for (Reference r : getReferencesTo(a)) {
            Address from = r.getFromAddress();
            Function f = getFunctionContaining(from);
            MemoryBlock b = currentProgram.getMemory().getBlock(from);
            out.println("   " + from + "  type=" + r.getReferenceType()
                        + "  in " + (f != null ? f.getName() : (b != null ? "block " + b.getName() : "?")));
            if (++n >= 40) { out.println("   ... (capped at 40)"); break; }
        }
        if (n == 0) out.println("   (none)");
        out.println();
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/compactor_family.txt", "UTF-8");

        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        // The family.
        decompile(dec, "0x00b47ac0");   // wrapper A (sets +0x50, arg [this+0x4c])
        decompile(dec, "0x00b47af0");   // wrapper B (checks +0x51)
        decompile(dec, "0x00b472f0");   // wrapper B's alternative target
        decompile(dec, "0x00b46c20");   // the compactor itself
        decompile(dec, "0x00b45f80");   // sibling flagged in the 08-13 survey

        // Vtable hunt: the wrappers are indirect/vtable targets, so data refs
        // to their entry addresses are vtable slots; refs to the vtable are
        // constructors.
        refsTo("0x00b47ac0", "wrapper A - data refs = vtable slots");
        refsTo("0x00b47af0", "wrapper B - data refs = vtable slots");
        refsTo("0x00b46c20", "compactor direct callers");
        refsTo("0x00b472f0", "alt target callers");

        dec.dispose();
        out.close();
        println("done -> compactor_family.txt");
    }
}
