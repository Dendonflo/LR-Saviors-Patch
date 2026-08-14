import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// The debug-menu manager (DAT_024c3d74) is built unconditionally at boot by
// FUN_004bc200; retail merely never sets its enable bits (byte +6 |= 2|4).
// Decompile the ctor, read its vtable, and decompile the per-frame virtual
// at +0x84 plus the boot-time one at +0x40 - these show what the bits gate
// and whether setting them at runtime is sufficient to open the menu.
public class DebugMgrClass extends GhidraScript {
    PrintWriter out;
    DecompInterface dec;
    Set<String> done = new HashSet<>();

    void decomp(Function f, String why) {
        if (f == null) { out.println("### (" + why + "): null"); return; }
        if (!done.add(f.getEntryPoint().toString())) return;
        out.println("############################################################");
        out.println("### " + why + " : " + f.getName() + " @ " + f.getEntryPoint());
        DecompileResults res = dec.decompileFunction(f, 120, new ConsoleTaskMonitor());
        if (res != null && res.decompileCompleted())
            out.println(res.getDecompiledFunction().getC());
        else out.println("  (decompile failed)");
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/debug_mgr_class.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        ghidra.program.model.mem.Memory mem = currentProgram.getMemory();

        Function ctor = getFunctionAt(toAddr(0x4bc200L));
        decomp(ctor, "debug menu manager ctor (DAT_024c3d74 = this)");

        // find the vtable the ctor installs: first MOV [reg], imm32 into .rdata/.data
        Address vt = null;
        if (ctor != null) {
            InstructionIterator ii = currentProgram.getListing().getInstructions(ctor.getBody(), true);
            while (ii.hasNext()) {
                Instruction ins = ii.next();
                for (Reference r : ins.getReferencesFrom()) {
                    long o = r.getToAddress().getOffset();
                    if (r.isMemoryReference() && o >= 0xde9000L && o < 0x2289000L) {
                        // candidate vtable pointer: dereference slot 0 and see if code
                        try {
                            long s0 = mem.getInt(r.getToAddress()) & 0xffffffffL;
                            if (s0 >= 0x401000L && s0 < 0xde8000L) { vt = r.getToAddress(); break; }
                        } catch (Exception e) {}
                    }
                }
                if (vt != null) break;
            }
        }
        out.println("=== vtable candidate: " + vt);
        if (vt != null) {
            for (int off = 0; off <= 0x90; off += 4) {
                long p = mem.getInt(vt.add(off)) & 0xffffffffL;
                Function f = getFunctionAt(toAddr(p));
                out.println("  +0x" + Integer.toHexString(off) + " -> " + Long.toHexString(p)
                            + (f != null ? " " + f.getName() : ""));
            }
            long tick = mem.getInt(vt.add(0x84)) & 0xffffffffL;
            long init = mem.getInt(vt.add(0x40)) & 0xffffffffL;
            decomp(getFunctionAt(toAddr(tick)), "manager virtual +0x84 (per-frame tick)");
            decomp(getFunctionAt(toAddr(init)), "manager virtual +0x40 (called at boot end)");
        }
        out.close();
        println("done");
    }
}
