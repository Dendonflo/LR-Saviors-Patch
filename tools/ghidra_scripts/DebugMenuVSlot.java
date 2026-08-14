import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// INTERVAL_DEBUG_MENU_WHITE's handler is "MOV EAX,[ECX]; JMP [EAX+0x164]" -
// AppGame virtual slot 0x164. Locate white::AppGame::vftable, resolve that
// slot (plus 0xe8/0xf8/0x108 used by the neighbouring thunks for context),
// and decompile the targets.
public class DebugMenuVSlot extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/debug_menu_vslot.txt", "UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        SymbolTable st = currentProgram.getSymbolTable();
        Address vt = null;
        for (Symbol s : st.getDefinedSymbols()) {
            String n = s.getName(true);
            if (n.contains("AppGame") && n.contains("vftable") && !n.contains("ISaveData")) {
                out.println("symbol: " + n + " @ " + s.getAddress());
                if (vt == null || n.equals("white::AppGame::vftable")) vt = s.getAddress();
            }
        }
        if (vt == null) { out.println("vftable not found"); out.close(); return; }
        ghidra.program.model.mem.Memory mem = currentProgram.getMemory();
        int[] slots = { 0xe8, 0xf8, 0x108, 0x164 };
        for (int off : slots) {
            long p = mem.getInt(vt.add(off)) & 0xffffffffL;
            Function f = getFunctionAt(toAddr(p));
            out.println("### vtable+0x" + Integer.toHexString(off) + " -> " + Long.toHexString(p)
                        + (f != null ? " " + f.getName() : " (no function)"));
            if (f != null && off == 0x164) {
                DecompileResults res = dec.decompileFunction(f, 90, new ConsoleTaskMonitor());
                if (res != null && res.decompileCompleted())
                    out.println(res.getDecompiledFunction().getC());
            }
        }
        out.close();
        println("done");
    }
}
