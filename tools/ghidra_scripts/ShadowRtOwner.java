import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Walking UP from CreateTexture has now hit four layers of generic engine
// plumbing (FUN_00aa3960 creator, FUN_00aa3ce0 TextureImp ctor, FUN_00a94770
// texture factory with five callers). Each probe cost a game run and found
// more plumbing.
//
// Come at it from the other end instead. FUN_00ac6b00 (DRAW_MULTI_SAMPLE_SHADOW,
// the pass that renders into the 1920x1080 targets) reads its surfaces out of
// DAT_05115724:
//     DAT_0510e2e0 = *(*(DAT_05115724 + 0x84) + 0x30);
//     ... +0x8c, +0x70, +0x74, +0x80, +0x94 ...
// So whoever WRITES those fields is the screen-space shadow allocator - the
// analogue of FUN_00b010c0 for the shadow maps, which was found exactly this
// way (FEATURES.md, "Decomp round 1").
//
// Two sweeps, because a pointer-reached object has no literal to scan for:
//   1. direct references to 05115724
//   2. every instruction carrying 0x84 / 0x8c / 0x70 / 0x74 / 0x80 / 0x94 as a
//      displacement in a STORE - catches the allocator even if it reaches the
//      object through a register
public class ShadowRtOwner extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/shadow_rt_owner.txt","UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();

        out.println("################ direct refs to 05115724 ################");
        Address g = currentProgram.getAddressFactory().getAddress("05115724");
        Set<Function> touchers = new LinkedHashSet<>();
        for (Reference r : getReferencesTo(g)) {
            Instruction i = lst.getInstructionAt(r.getFromAddress());
            Function f = getFunctionContaining(r.getFromAddress());
            out.printf("  %-12s %-30s %s%n", r.getFromAddress(),
                       f == null ? "(none)" : f.getName(), i == null ? "" : i.toString());
            if (f != null) touchers.add(f);
        }

        out.println();
        out.println("################ functions STORING into +0x84 / +0x8c (the RT slots) ################");
        Set<Function> writers = new LinkedHashSet<>();
        InstructionIterator it = lst.getInstructions(true);
        while (it.hasNext()) {
            Instruction i = it.next();
            String t = i.toString();
            if (!t.startsWith("MOV ")) continue;
            // store form: MOV dword ptr [REG + 0xNN],SRC  (']' before the comma)
            int rb = t.indexOf(']'), cm = t.indexOf(',');
            if (rb < 0 || cm < 0 || rb > cm) continue;
            if (!(t.contains("+ 0x84]") || t.contains("+ 0x8c]"))) continue;
            Function f = getFunctionContaining(i.getAddress());
            if (f == null) continue;
            if (writers.add(f))
                out.printf("  %-12s %s   %s%n", i.getAddress(), f.getName(), t);
        }

        out.println();
        out.println("################ decompiled: functions that BOTH touch 05115724 and store to those slots ################");
        for (Function f : writers) {
            if (!touchers.contains(f)) continue;
            out.println("======== " + f.getName() + " @ " + f.getEntryPoint() + " ========");
            DecompileResults r = dec.decompileFunction(f, 180, new ConsoleTaskMonitor());
            String c = (r != null && r.getDecompiledFunction() != null)
                       ? r.getDecompiledFunction().getC() : "(failed)";
            if (c.length() > 12000) c = c.substring(0, 12000) + "\n... [truncated]";
            out.println(c);
        }
        out.close();
        println("wrote shadow_rt_owner.txt");
    }
}
