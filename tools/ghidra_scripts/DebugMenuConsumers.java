import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Experiment 1 result: the enable bits were written and STUCK (one apply, no
// re-assert) but nothing changed in game. So the bits are not the whole gate.
// Find what actually consumes them.
//
// 1. The AppGame debug-menu accessor family (0x42a2c0..0x42a750) - every
//    function that reads DAT_024c3d74. These are the API surface; one of them
//    should be "is the menu open/enabled".
// 2. The real per-frame body: manager vslot 0x84 -> FUN_004bc090 ->
//    FUN_0065dc30 -> FUN_006b59c0.
// 3. The init bodies: vslot 0x40 -> FUN_00666400 -> FUN_006fd340 + FUN_006651a0.
//    If these registered pages only when the bits were set, our late poke
//    missed the window entirely - that is the leading hypothesis.
// 4. Every instruction testing AppGame+0x680 / +0x684 / +0x688, so we can see
//    which bits are read at runtime versus only at boot.
// 5. DebugMenuHidInterface - the input path. If its callback is registered
//    inside a boot-time gated block, no combo can ever open the menu.
public class DebugMenuConsumers extends GhidraScript {
    PrintWriter out;
    DecompInterface dec;
    Set<String> done = new HashSet<>();

    void decomp(Function f, String why) {
        if (f == null) { out.println("### (" + why + "): null"); return; }
        if (!done.add(f.getEntryPoint().toString())) {
            out.println("### (" + why + "): already dumped " + f.getName());
            return;
        }
        out.println("############################################################");
        out.println("### " + why + " : " + f.getName() + " @ " + f.getEntryPoint());
        out.print("### callers:");
        int n = 0;
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            Function cf = getFunctionContaining(r.getFromAddress());
            if (cf != null) { out.print(" " + cf.getName()); if (++n > 10) { out.print(" ..."); break; } }
        }
        out.println();
        DecompileResults res = dec.decompileFunction(f, 120, new ConsoleTaskMonitor());
        if (res != null && res.decompileCompleted())
            out.println(res.getDecompiledFunction().getC());
        else out.println("  (decompile failed)");
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/debug_menu_consumers.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        ghidra.program.model.mem.Memory mem = currentProgram.getMemory();
        Listing lst = currentProgram.getListing();

        // ---- 1. everything that reads the manager singleton -----------------
        out.println("================ AppGame debug-menu accessor family ================");
        Set<Address> fns = new LinkedHashSet<>();
        for (Reference r : getReferencesTo(toAddr(0x24c3d74L))) {
            Function f = getFunctionContaining(r.getFromAddress());
            if (f != null) fns.add(f.getEntryPoint());
        }
        for (Address a : fns) {
            Function f = getFunctionAt(a);
            // the two boot giants are already dumped elsewhere
            if (a.getOffset() == 0x432200L || a.getOffset() == 0x42c250L) {
                out.println("### skipping boot giant " + f.getName());
                continue;
            }
            decomp(f, "reads DAT_024c3d74");
        }

        // ---- 2. the real tick body -----------------------------------------
        decomp(getFunctionAt(toAddr(0x6b59c0L)), "TICK BODY (mgr vslot 0x84 -> 0065dc30 -> here)");

        // ---- 3. the init bodies --------------------------------------------
        decomp(getFunctionAt(toAddr(0x6fd340L)), "INIT part 1 (mgr vslot 0x40 -> 00666400 -> here)");
        decomp(getFunctionAt(toAddr(0x6651a0L)), "INIT part 2");

        // ---- 4. who tests the AppGame flag dwords ---------------------------
        out.println("================ instructions touching AppGame+0x680/684/688 ================");
        InstructionIterator ii = lst.getInstructions(true);
        Map<String, Integer> perFunc = new LinkedHashMap<>();
        while (ii.hasNext() && !monitor.isCancelled()) {
            Instruction ins = ii.next();
            String s = ins.toString();
            if (!(s.contains("0x680") || s.contains("0x684") || s.contains("0x688"))) continue;
            if (!s.contains("[")) continue;
            Function f = getFunctionContaining(ins.getAddress());
            String fn = f == null ? "(none)" : f.getName();
            out.println("  " + ins.getAddress() + ": " + s + "   in " + fn);
            perFunc.merge(fn, 1, Integer::sum);
        }
        out.println("--- per-function counts: " + perFunc);

        // ---- 5. the HID / input path ---------------------------------------
        out.println("================ DebugMenuHidInterface ================");
        // find its RTTI type-descriptor string, then the vtables/meta near it
        DataIterator di = lst.getDefinedData(true);
        List<Address> hid = new ArrayList<>();
        while (di.hasNext()) {
            Data d = di.next();
            Object v = d.getValue();
            if (v == null) continue;
            String s = v.toString();
            if (s.contains("DebugMenuHidInterface") || s.contains("DebugMenuPageCommon")
                || s.contains("CheatPadWindow")) {
                out.println("  string " + d.getAddress() + " \"" + s + "\"");
                for (Reference r : getReferencesTo(d.getAddress())) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    out.println("      ref " + r.getFromAddress()
                        + (f != null ? "  in " + f.getName() + "@" + f.getEntryPoint() : "  (data)"));
                    if (f != null) hid.add(f.getEntryPoint());
                }
            }
        }
        for (Address a : hid) decomp(getFunctionAt(a), "HID/page-related");

        out.close();
        println("done");
    }
}
