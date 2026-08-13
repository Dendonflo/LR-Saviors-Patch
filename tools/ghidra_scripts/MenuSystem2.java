import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.mem.Memory;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Second pass on the Win32 menu system. Known from pass 1 (menu_system.txt):
//   manager obj: +0x8 root HMENU, +0x44 cur HMENU, +0x48 pos, +0x4c id ctr,
//   +0xc label-source for FUN_00ac9620(name)->LPWSTR
//   FUN_00abc3c0 puts the handler fn-ptr in dwItemData and calls handler(0)
//   for the checkmark; FUN_00abc310 = clear + rebuild + settings[0x16e]=1.
// Still needed before the hook can be written:
//   A. where FUN_00acaf60 gets `this` (global singleton? passed ECX?)
//      -> raw disassembly of its prologue + around first builder call
//   B. FUN_00ac9620: label lookup, and its behaviour for UNKNOWN names
//   C. the WM_COMMAND dispatcher that reads dwItemData back and calls the
//      handler (search GetMenuItemInfo refs + the WndProc via FUN_00d129a0)
//   D. handler protocol confirmation: decompile two real handlers fully
//      (Presentation_FullScreen FUN_00aca830, Scaling_Advanced FUN_00acaea0)
//   E. who calls FUN_00abc310 (what triggers rebuilds)
//   F. the 0xC-stride data table at 0218b4b8..0218b620 whose rows point at
//      the Graphics_* name strings (suspected name->label / name->text-id)
public class MenuSystem2 extends GhidraScript {
    private DecompInterface dec;
    private PrintWriter out;
    private Set<String> seen = new HashSet<>();

    private void dump(String addr, String why) {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        Function f = getFunctionContaining(a);
        if (f == null) { out.println("(no function at " + addr + " - " + why + ")"); return; }
        if (!seen.add(f.getEntryPoint().toString())) {
            out.println("(already dumped " + f.getName() + " - " + why + ")"); return;
        }
        out.println("################ " + f.getName() + " @ " + f.getEntryPoint()
                    + "   (" + why + ") ################");
        DecompileResults r = dec.decompileFunction(f, 240, new ConsoleTaskMonitor());
        String c = (r != null && r.getDecompiledFunction() != null)
                   ? r.getDecompiledFunction().getC() : "(decompile failed)";
        if (c.length() > 20000) c = c.substring(0, 20000) + "\n... [truncated]";
        out.println(c);
        out.println();
    }

    private void disasm(String addr, int count, String why) {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        out.println("-------- disasm " + addr + " x" + count + "  (" + why + ") --------");
        Listing lst = currentProgram.getListing();
        Instruction ins = lst.getInstructionAt(a);
        if (ins == null) ins = lst.getInstructionContaining(a);
        for (int i = 0; i < count && ins != null; i++) {
            out.println("   " + ins.getAddress() + "  " + ins);
            ins = ins.getNext();
        }
        out.println();
    }

    private void refs(String addr, String label) {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        out.println("======== references to " + label + " (" + addr + ") ========");
        for (Reference r : getReferencesTo(a)) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.println("   " + r.getFromAddress()
                        + (f == null ? "" : "  in " + f.getName() + " @ " + f.getEntryPoint())
                        + "  type=" + r.getReferenceType());
        }
        out.println();
    }

    private String strAt(Address a) {
        try {
            Memory m = currentProgram.getMemory();
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 64; i++) {
                byte b = m.getByte(a.add(i));
                if (b == 0) break;
                if (b < 0x20 || b > 0x7e) return null;
                sb.append((char) b);
            }
            return sb.length() > 0 ? sb.toString() : null;
        } catch (Exception e) { return null; }
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/menu_system2.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        // A. this-pointer source in the tree builder + exact conventions of
        //    the builder functions (for calling them from the hook DLL)
        disasm("00acaf60", 30, "FUN_00acaf60 prologue: where does `this` come from");
        disasm("00abc350", 10, "open-popup prologue: convention check");
        disasm("00abc3c0", 12, "add-item prologue: convention check");
        disasm("00abc4b0", 8,  "close-popup prologue: convention check");
        disasm("00abc310", 14, "rebuild: convention check");

        // B. label lookup + behaviour on unknown names
        dump("00ac9620", "label lookup: name -> LPWSTR");
        refs("00ac9620", "label lookup callers");

        // C. WM_COMMAND dispatch: whoever reads MIIM_DATA back out
        String[] apis = { "GetMenuItemInfoA", "GetMenuItemInfoW",
                          "DefWindowProcA", "DefWindowProcW" };
        SymbolTable st = currentProgram.getSymbolTable();
        Set<Function> users = new LinkedHashSet<>();
        for (String api : apis) {
            out.printf("=== import %s ===%n", api);
            SymbolIterator it = st.getAllSymbols(true);
            while (it.hasNext()) {
                Symbol s = it.next();
                if (!s.getName().equals(api)) continue;
                for (Reference r : getReferencesTo(s.getAddress())) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    out.printf("      ref %s  %s  type=%s%n", r.getFromAddress(),
                        f != null ? "in " + f.getName() + " @ " + f.getEntryPoint() : "NO FUNC",
                        r.getReferenceType());
                    if (f != null) users.add(f);
                }
            }
        }
        out.println();
        for (Function f : users) {
            if (f.getBody().getNumAddresses() > 15000) {
                out.println("(skipping huge " + f.getName() + ")"); continue;
            }
            dump(f.getEntryPoint().toString(), "GetMenuItemInfo/DefWindowProc user, size="
                 + f.getBody().getNumAddresses());
        }

        // window creator - registers the WndProc
        dump("00d129a0", "window creator (returns HWND) - find lpfnWndProc");

        // D. two real handlers, full decompile: confirm query(0)/apply(x)
        dump("00aca830", "handler Graphics_Presentation_FullScreen");
        dump("00acaea0", "handler Graphics_Scaling_Advanced");
        dump("00acac90", "handler Graphics_Shadowing_Advanced");

        // E. rebuild triggers
        refs("00abc310", "FUN_00abc310 (menu rebuild)");
        for (Reference r : getReferencesTo(
                 currentProgram.getAddressFactory().getAddress("00abc310"))) {
            Function f = getFunctionContaining(r.getFromAddress());
            if (f != null && f.getBody().getNumAddresses() < 8000)
                dump(f.getEntryPoint().toString(), "calls the menu rebuild");
        }

        // F. the 0xC-stride table around 0218b4b8..0218b620: dump dwords,
        //    resolving any that point at readable ASCII
        out.println("======== data table 0218b490..0218b640 (dwords, strings resolved) ========");
        Memory mem = currentProgram.getMemory();
        Address base = currentProgram.getAddressFactory().getAddress("0218b490");
        for (int off = 0; off < 0x1b0; off += 4) {
            Address a = base.add(off);
            int v = mem.getInt(a);
            String s = null;
            try {
                Address t = currentProgram.getAddressFactory()
                            .getAddress(String.format("%08x", v));
                s = strAt(t);
            } catch (Exception e) {}
            out.printf("   %s : %08x%s%n", a, v, s != null ? "  -> \"" + s + "\"" : "");
        }
        // and who references the table start
        refs("0218b4b8", "suspected table row (Presentation)");
        refs("0218b554", "suspected table row (Scaling)");

        out.close();
        println("wrote menu_system2.txt");
    }
}
