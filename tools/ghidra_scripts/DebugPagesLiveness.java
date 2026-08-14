import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.StringDataType;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// DECISIVE TEST. Experiment 1 set the enable bits successfully (verified in
// log) with no visible effect, and the object I assumed was the debug menu
// manager (DAT_024c3d74) ticks into ScenePathSearch.cpp - so that
// identification was wrong.
//
// The question that actually matters: is the DebugMenuPage machinery LIVE
// CODE (constructors called from somewhere, merely gated) or DEAD CODE
// (vtables + RTTI retained by the linker, but nothing ever instantiates
// them)? An early hint: the page-id string "debug_menu_common" has NO
// references at all.
//
// Method, per DebugMenuPage* class:
//   type descriptor string -> vtable(s) whose meta-ptr names it
//     -> references TO the vtable = its constructors
//       -> references TO those constructors = who instantiates the page
// A class with zero instantiators is dead: no bit anywhere can revive it.
//
// Also reports reference counts for every debug_menu_* / DebugMenu id string,
// and identifies what class DAT_024c3d74's vtable (0206e814) really belongs to.
public class DebugPagesLiveness extends GhidraScript {

    PrintWriter out;
    ghidra.program.model.mem.Memory mem;

    // find the RTTI type-descriptor data whose string matches, return its address
    List<Address> typeDescriptors(String needle) {
        List<Address> r = new ArrayList<>();
        DataIterator di = currentProgram.getListing().getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            Object v = d.getValue();
            if (v == null) continue;
            String s = v.toString();
            if (s.startsWith(".?AV") && s.contains(needle)) r.add(d.getAddress());
        }
        return r;
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/debug_pages_liveness.txt", "UTF-8");
        mem = currentProgram.getMemory();

        // ---- what is 0206e814 really? ------------------------------------
        out.println("================ identity of the 'manager' vtable 0206e814 ================");
        for (long a = 0x206e800L; a <= 0x206e820L; a += 4) {
            Address ad = toAddr(a);
            int v = mem.getInt(ad);
            StringBuilder sb = new StringBuilder();
            for (Symbol s : currentProgram.getSymbolTable().getSymbols(ad))
                sb.append(" [").append(s.getName(true)).append("]");
            out.printf("  %s: %08x%s%n", ad, v, sb.toString());
        }

        // ---- liveness of every DebugMenuPage / debug class ----------------
        String[] groups = { "DebugMenuPage", "LoadViewer", "terminal@debug",
                            "CheatPadWindow", "OutlinerWindow", "DebugMenuHidInterface",
                            "DebugTimeScheduleManager", "SceneCameraControlByDebugPad" };
        for (String g : groups) {
            out.println();
            out.println("================ group: " + g + " ================");
            for (Address td : typeDescriptors(g)) {
                String name = "";
                Data d = currentProgram.getListing().getDefinedDataAt(td);
                if (d != null && d.getValue() != null) name = d.getValue().toString();

                // who references the type descriptor: RTTI structures
                List<Address> vtables = new ArrayList<>();
                int rttiRefs = 0;
                for (Reference r : getReferencesTo(td)) {
                    rttiRefs++;
                    // walk: typedesc <- COL <- (vtable-4). Find data refs to the COL.
                    for (Reference r2 : getReferencesTo(r.getFromAddress())) {
                        Address maybeMeta = r2.getFromAddress();
                        vtables.add(maybeMeta.add(4));   // vtable starts after meta ptr
                    }
                }
                // constructors = code refs to the vtable address
                int ctors = 0, instantiators = 0;
                Set<String> ctorNames = new LinkedHashSet<>();
                Set<String> instNames = new LinkedHashSet<>();
                for (Address vt : vtables) {
                    for (Reference r : getReferencesTo(vt)) {
                        Function f = getFunctionContaining(r.getFromAddress());
                        if (f == null) continue;
                        ctors++;
                        ctorNames.add(f.getName() + "@" + f.getEntryPoint());
                        for (Reference r2 : getReferencesTo(f.getEntryPoint())) {
                            Function cf = getFunctionContaining(r2.getFromAddress());
                            if (cf != null) { instantiators++; instNames.add(cf.getName() + "@" + cf.getEntryPoint()); }
                            else instNames.add("(data/vtable slot @" + r2.getFromAddress() + ")");
                        }
                    }
                }
                out.println("  " + name);
                out.println("      typedesc " + td + "  rttiRefs=" + rttiRefs
                            + "  vtables=" + vtables.size() + "  ctors=" + ctorNames.size()
                            + "  INSTANTIATORS=" + instNames.size());
                for (String s : ctorNames) out.println("        ctor: " + s);
                for (String s : instNames) out.println("        inst: " + s);
            }
        }

        // ---- reference counts for the page-id strings ---------------------
        out.println();
        out.println("================ debug menu id strings: reference counts ================");
        DataIterator di = currentProgram.getListing().getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            if (!(d.getDataType() instanceof StringDataType)) continue;
            Object v = d.getValue();
            if (v == null) continue;
            String s = v.toString();
            String l = s.toLowerCase();
            if (!(l.contains("debug_menu") || l.contains("debug_battle_menu")
                  || l.contains("state_debug") || l.contains("interval_debug")
                  || l.contains("draw_debug"))) continue;
            int code = 0, data = 0;
            StringBuilder where = new StringBuilder();
            for (Reference r : getReferencesTo(d.getAddress())) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f != null) { code++; if (where.length() < 200) where.append(" ").append(f.getName()); }
                else { data++; if (where.length() < 200) where.append(" data@").append(r.getFromAddress()); }
            }
            out.printf("  %-40s code=%d data=%d %s%n", "\"" + s + "\"", code, data, where);
        }

        out.close();
        println("done");
    }
}
