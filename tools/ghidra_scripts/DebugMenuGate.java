import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.RefType;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Follow-up to DebugMenuSurvey: FUN_00ccea50's return decides the
// "(DebugComponentEnabled)" window title. Decompile it, find every global it
// reads, and decompile everything that WRITES those globals - that is the
// enable chain. Also: the INTERVAL_DEBUG_MENU_WHITE handler (LAB_00427dc0),
// the PrototypeDebugMenu spawner's caller (FUN_008ce2d0) and thread entry
// (FUN_008cdbb0), and the STATE_DEBUG_MENU descriptor at 0x22b1648.
public class DebugMenuGate extends GhidraScript {

    DecompInterface dec;
    PrintWriter out;
    Set<String> done = new HashSet<>();

    void decomp(Function f, String why) {
        if (f == null) { out.println("### (" + why + "): NULL FUNCTION"); return; }
        if (!done.add(f.getEntryPoint().toString())) return;
        out.println("############################################################");
        out.println("### " + why + " : " + f.getName() + " @ " + f.getEntryPoint());
        out.print("### callers:");
        int n = 0;
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            Function cf = getFunctionContaining(r.getFromAddress());
            if (cf != null) { out.print(" " + cf.getName()); if (++n > 10) { out.print(" ..."); break; } }
        }
        out.println();
        DecompileResults res = dec.decompileFunction(f, 90, new ConsoleTaskMonitor());
        if (res != null && res.decompileCompleted())
            out.println(res.getDecompiledFunction().getC());
        else out.println("  (decompile failed)");
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/debug_menu_gate.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();

        Function pred = getFunctionAt(toAddr(0xccea50L));
        decomp(pred, "gate predicate (decides DebugComponentEnabled title)");

        // globals the predicate touches, and their writers
        if (pred != null) {
            Set<Address> globals = new LinkedHashSet<>();
            InstructionIterator ii = lst.getInstructions(pred.getBody(), true);
            while (ii.hasNext()) {
                Instruction ins = ii.next();
                for (Reference r : ins.getReferencesFrom()) {
                    Address t = r.getToAddress();
                    if (r.isMemoryReference() && t.getOffset() >= 0x2289000L + 0x400000L - 0x400000L
                        && !r.getReferenceType().isCall() && t.getOffset() > 0xde9000L)
                        globals.add(t);
                }
            }
            for (Address g : globals) {
                out.println("=== global " + g + " referenced by predicate; all refs:");
                int n = 0;
                for (Reference r : getReferencesTo(g)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    out.println("    " + r.getFromAddress() + " " + r.getReferenceType()
                        + (f == null ? "" : "  in " + f.getName() + "@" + f.getEntryPoint()));
                    if (++n > 40) { out.println("    ..."); break; }
                }
                // decompile writers
                for (Reference r : getReferencesTo(g)) {
                    if (r.getReferenceType().isWrite()) {
                        Function f = getFunctionContaining(r.getFromAddress());
                        decomp(f, "WRITER of " + g);
                    }
                }
            }
        }

        // interval handler + prototype menu chain
        decomp(getFunctionContaining(toAddr(0x427dc0L)), "INTERVAL_DEBUG_MENU_WHITE handler");
        decomp(getFunctionAt(toAddr(0x8ce2d0L)), "caller of PrototypeDebugMenu spawner");
        decomp(getFunctionAt(toAddr(0x8cdbb0L)), "PrototypeDebugMenu thread entry");

        // STATE_DEBUG_MENU descriptor: dump 16 dwords around 0x22b1648 and
        // decompile any that are code pointers
        out.println("############################################################");
        out.println("### STATE_DEBUG_MENU descriptor region @ 022b1648");
        ghidra.program.model.mem.Memory mem = currentProgram.getMemory();
        for (int i = -8; i < 16; i++) {
            Address a = toAddr(0x22b1648L + i * 4L);
            int v = mem.getInt(a);
            String note = "";
            Address t = toAddr(v & 0xffffffffL);
            Function f = null;
            if ((v & 0xffffffffL) >= 0x401000L && (v & 0xffffffffL) < 0xde8000L) {
                f = getFunctionContaining(t);
                note = "  -> code " + (f != null ? f.getName() + "@" + f.getEntryPoint() : "(no func)");
            } else if ((v & 0xffffffffL) >= 0xde9000L && (v & 0xffffffffL) < 0x2289000L) {
                try {
                    ghidra.program.model.data.StringDataInstance sdi = null;
                    byte[] b = new byte[48];
                    mem.getBytes(t, b);
                    int len = 0; while (len < 48 && b[len] >= 0x20 && b[len] < 0x7f) len++;
                    if (len >= 3) note = "  -> str \"" + new String(b, 0, len) + "\"";
                } catch (Exception e) {}
            }
            out.printf("  %s: %08x%s%n", a, v, note);
        }
        // decompile the state's handler functions
        for (int i = -8; i < 16; i++) {
            Address a = toAddr(0x22b1648L + i * 4L);
            int v = mem.getInt(a);
            if ((v & 0xffffffffL) >= 0x401000L && (v & 0xffffffffL) < 0xde8000L) {
                Function f = getFunctionContaining(toAddr(v & 0xffffffffL));
                decomp(f, "STATE_DEBUG_MENU table code ptr " + Integer.toHexString(v));
            }
        }

        out.close();
        println("done");
    }
}
