import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.StringDataType;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// GOAL (practical, for the shipping mod): a reliable runtime "a cutscene is
// playing" signal, so ShadowSplitNear (the shadow-distance option) can be
// forced back to default during cinema and restored afterwards. Cutscenes use
// hand-tuned shadow setups and our cascade override breaks some of them.
//
// Candidates found by string survey:
//   A. a cutscene-PLAYER state machine: STATE_PREFETCH / LOAD_WAIT /
//      START_WAIT / PLAY / PLAY_END_WAIT / SKIP* / END
//   B. ZoneStateMachine: STATE_EVENT_ON_FIELD_* / EVENT_ON_BATTLE_* /
//      LIVE_EVENT_ON_FIELD_* (cutscene-during-field/battle)
//   C. white::cinema::CinemaController, which has a LIVE registered
//      AppGameFrameInterface vtable at 0x020e936c, plus
//      "[CinemaController] PAUSE ON/OFF" log calls.
//
// For each: locate the name table, find the code that indexes it (typically a
// state->name helper used by logging), and decompile it plus its callers -
// that is what exposes the state VARIABLE we can poll from the monitor thread.
public class CutsceneDetect extends GhidraScript {
    PrintWriter out;
    DecompInterface dec;
    Set<String> dumped = new HashSet<>();

    void decomp(Function f, String why) {
        if (f == null) { out.println("### (" + why + "): null"); return; }
        if (!dumped.add(f.getEntryPoint().toString())) {
            out.println("### (" + why + "): already dumped " + f.getName()); return;
        }
        out.println("############################################################");
        out.println("### " + why + " : " + f.getName() + " @ " + f.getEntryPoint());
        out.print("### callers:");
        int n = 0;
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            Function cf = getFunctionContaining(r.getFromAddress());
            out.print(cf != null ? " " + cf.getName() + "@" + cf.getEntryPoint()
                                 : " (data@" + r.getFromAddress() + ")");
            if (++n > 10) { out.print(" ..."); break; }
        }
        out.println();
        DecompileResults res = dec.decompileFunction(f, 120, new ConsoleTaskMonitor());
        if (res != null && res.decompileCompleted())
            out.println(res.getDecompiledFunction().getC());
        else out.println("  (decompile failed)");
    }

    // find a string's address by exact value
    Address findStr(String want) {
        DataIterator di = currentProgram.getListing().getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            if (!(d.getDataType() instanceof StringDataType)) continue;
            Object v = d.getValue();
            if (v != null && want.equals(v.toString())) return d.getAddress();
        }
        return null;
    }

    // report who points at this string, and dump the enclosing table neighbourhood
    void tableProbe(String name, String label) {
        Address a = findStr(name);
        out.println("--- \"" + name + "\" (" + label + ") @ " + a);
        if (a == null) return;
        for (Reference r : getReferencesTo(a)) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.println("      ptr at " + r.getFromAddress()
                + (f != null ? "  CODE in " + f.getName() + "@" + f.getEntryPoint() : "  (table slot)"));
            if (f == null) {
                // it is a table slot: who reads the table?
                for (Reference rr : getReferencesTo(r.getFromAddress())) {
                    Function rf = getFunctionContaining(rr.getFromAddress());
                    out.println("         table read by " + rr.getFromAddress()
                        + (rf != null ? " in " + rf.getName() + "@" + rf.getEntryPoint() : ""));
                    if (rf != null) decomp(rf, "reads " + label + " name table");
                }
            } else decomp(f, "uses " + label + " name");
        }
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/cutscene_detect.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        ghidra.program.model.mem.Memory mem = currentProgram.getMemory();

        out.println("================ A. cutscene player state machine ================");
        for (String s : new String[]{ "STATE_PLAY", "STATE_PREFETCH", "STATE_PLAY_END_WAIT",
                                      "STATE_START_WAIT", "STATE_SKIP" })
            tableProbe(s, "cutscene-player");

        out.println();
        out.println("================ B. ZoneStateMachine event states ================");
        for (String s : new String[]{ "STATE_EVENT_ON_FIELD_IDLE", "STATE_EVENT_ON_BATTLE_IDLE",
                                      "STATE_LIVE_EVENT_ON_FIELD_IDLE", "STATE_FIELD_IDLE" })
            tableProbe(s, "zone-state");

        out.println();
        out.println("================ C. CinemaController ================");
        for (String s : new String[]{ "[CinemaController] PAUSE ON", "[CinemaController] PAUSE OFF" })
            tableProbe(s, "cinema-pause");

        // the live registered frame interface vtable
        out.println("--- AppGameFrameInterface<CinemaController>::vftable @ 020e936c");
        for (int off = 0; off <= 0x40; off += 4) {
            long p = mem.getInt(toAddr(0x20e936cL + off)) & 0xffffffffL;
            Function f = (p >= 0x401000L && p < 0xde8000L) ? getFunctionAt(toAddr(p)) : null;
            out.println("   +0x" + Integer.toHexString(off) + " -> " + Long.toHexString(p)
                        + (f != null ? " " + f.getName() : ""));
        }
        out.println("--- refs to that vtable (constructors):");
        for (Reference r : getReferencesTo(toAddr(0x20e936cL))) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.println("   " + r.getFromAddress() + (f != null ? " in " + f.getName() + "@" + f.getEntryPoint() : ""));
            if (f != null) decomp(f, "CinemaController frame-interface ctor");
        }

        out.close();
        println("done");
    }
}
