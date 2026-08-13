import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// The game's windowed-mode Win32 menu bar (File / Graphics / Control /
// [Debug]) is built by FUN_00acaf60 through a tiny builder API:
//     FUN_00abc350(name)          open popup named <name>
//     FUN_00abc450()              ? (called right after every open)
//     FUN_00abc3c0(name, handler) add item, plain function-pointer callback
//     FUN_00abc4b0()              close popup
// The user wants the mod's own options injected INTO this menu (new
// "Optimization" popups + new/replacement entries under Graphics). Before
// hooking, four things must be understood, all answered by this dump:
//   1. What object the builders mutate (is there a singleton "menu manager"?)
//   2. How <name> becomes the displayed label (our names aren't in the
//      game's string table - is there a fallback? a lookup we must feed?)
//   3. Where the tree is materialised into a real HMENU
//      (CreatePopupMenu/AppendMenu/SetMenu) and WHEN (before/after device
//      init - decides where our hook must run).
//   4. How WM_COMMAND finds the handler and how checkmarks are driven
//      (CheckMenuItem/CheckMenuRadioItem) - decides whether radio state on
//      our items is automatic or ours to maintain.
public class MenuSystem extends GhidraScript {
    private DecompInterface dec;
    private PrintWriter out;
    private Set<String> seen = new HashSet<>();

    private void dump(String addr, String why) {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        Function f = getFunctionContaining(a);
        if (f == null) { out.println("(no function at " + addr + " - " + why + ")"); return; }
        if (!seen.add(f.getEntryPoint().toString())) {
            out.println("(already dumped " + f.getName() + " - " + why + ")");
            return;
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

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/menu_system.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        // --- 1. the builder API itself ---
        dump("00abc350", "menu builder: open popup(name)");
        dump("00abc450", "menu builder: post-open call");
        dump("00abc3c0", "menu builder: add item(name, handler)");
        dump("00abc4b0", "menu builder: close popup");
        dump("00abc310", "neighbour that reads settings obj 0511558c");

        // --- 2. who calls the registration function, i.e. when the menu
        //        tree is built ---
        refs("00acaf60", "FUN_00acaf60 (menu tree construction)");
        for (Reference r : getReferencesTo(
                 currentProgram.getAddressFactory().getAddress("00acaf60"))) {
            Function f = getFunctionContaining(r.getFromAddress());
            if (f != null && f.getBody().getNumAddresses() < 6000)
                dump(f.getEntryPoint().toString(), "caller of menu construction");
        }

        // --- 3. Win32 menu API usage: where the tree becomes an HMENU ---
        String[] apis = {
            "CreateMenu", "CreatePopupMenu", "AppendMenuA", "AppendMenuW",
            "InsertMenuA", "InsertMenuW", "InsertMenuItemA", "InsertMenuItemW",
            "ModifyMenuA", "ModifyMenuW", "SetMenu", "GetMenu",
            "CheckMenuItem", "CheckMenuRadioItem", "EnableMenuItem",
            "DrawMenuBar", "DeleteMenu", "RemoveMenu", "GetSubMenu",
            "SetMenuItemInfoA", "SetMenuItemInfoW"
        };
        SymbolTable st = currentProgram.getSymbolTable();
        Set<Function> menuUsers = new LinkedHashSet<>();
        for (String api : apis) {
            out.printf("=== import %s ===%n", api);
            int hits = 0;
            SymbolIterator it = st.getAllSymbols(true);
            while (it.hasNext()) {
                Symbol s = it.next();
                if (!s.getName().equals(api)) continue;
                hits++;
                out.printf("  symbol @ %s (type=%s)%n", s.getAddress(), s.getSymbolType());
                for (Reference r : getReferencesTo(s.getAddress())) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    out.printf("      ref %s  %s  type=%s%n", r.getFromAddress(),
                        f != null ? "in " + f.getName() + " @ " + f.getEntryPoint() : "NO FUNC",
                        r.getReferenceType());
                    if (f != null) menuUsers.add(f);
                }
            }
            if (hits == 0) out.println("  (not in symbol table)");
        }
        out.println();

        // decompile every function that touches the menu APIs (these are the
        // materialiser, the checkmark driver, and likely the WM_COMMAND
        // dispatcher, all in one small family)
        for (Function f : menuUsers) {
            if (f.getBody().getNumAddresses() > 12000) {
                out.println("(skipping huge " + f.getName() + " size="
                            + f.getBody().getNumAddresses() + ")");
                continue;
            }
            dump(f.getEntryPoint().toString(),
                 "uses Win32 menu APIs, size=" + f.getBody().getNumAddresses());
        }

        out.close();
        println("wrote menu_system.txt");
    }
}
