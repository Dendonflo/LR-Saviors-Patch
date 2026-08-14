import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// The INTERVAL_DEBUG_MENU_WHITE entry registers thunk LAB_00427dc0 (not a
// defined function). Disassemble the thunk region, follow its target, and
// decompile the real tick. That tick is where "is the debug menu enabled"
// is decided at runtime - the last piece of the enable chain.
public class DebugTickStub extends GhidraScript {
    PrintWriter out;
    DecompInterface dec;
    Set<String> done = new HashSet<>();

    void decomp(Function f, String why) {
        if (f == null || !done.add(f.getEntryPoint().toString())) return;
        out.println("############################################################");
        out.println("### " + why + " : " + f.getName() + " @ " + f.getEntryPoint());
        DecompileResults res = dec.decompileFunction(f, 90, new ConsoleTaskMonitor());
        if (res != null && res.decompileCompleted())
            out.println(res.getDecompiledFunction().getC());
        else out.println("  (decompile failed)");
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/debug_tick.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();

        // raw listing of the thunk neighbourhood
        out.println("=== listing 00427dc0..00427e00 ===");
        Address a = toAddr(0x427dc0L);
        while (a.getOffset() < 0x427e00L) {
            Instruction ins = lst.getInstructionAt(a);
            if (ins == null) { out.println("  " + a + ": (no instruction)"); a = a.add(1); continue; }
            out.println("  " + a + ": " + ins);
            for (Reference r : ins.getReferencesFrom())
                if (r.getReferenceType().isCall() || r.getReferenceType().isJump()) {
                    Function f = getFunctionContaining(r.getToAddress());
                    out.println("      -> " + r.getToAddress() + (f != null ? " (" + f.getName() + ")" : ""));
                    decomp(f, "target of thunk at " + a);
                }
            a = a.add(ins.getLength());
        }
        // boot path that calls FUN_00432200
        decomp(getFunctionAt(toAddr(0x434670L)), "boot path FUN_00434670 (calls FUN_00432200, White.Debug ref)");
        out.close();
        println("done");
    }
}
