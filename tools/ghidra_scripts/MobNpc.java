import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.data.StringDataType;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Mob ("random") NPC population: how many are kept alive, and what caps it.
// Strings: "MobNpc" (FUN_0061f150), "sMobNpcListId"/"u5NpcCount" (per-area
// table schema), "POPWNearLenMob"/"POPMobBase" consumers. Decompile the
// referencing functions and their callers one level up, and list every
// string containing "Mob"/"Npc" with its referencing function.
public class MobNpc extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/mob_npc.txt", "UTF-8");
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();
        Map<String, Function> todo = new TreeMap<>();
        out.println("======== strings ========");
        DataIterator di = lst.getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            if (!(d.getDataType() instanceof StringDataType)) continue;
            Object v = d.getValue(); if (v == null) continue;
            String s = v.toString();
            if (s.length() < 4 || s.length() > 60) continue;
            String u = s.toLowerCase();
            if (!(u.contains("mobnpc") || u.contains("npccount") || u.contains("mob_") || u.equals("mobnpc") || u.contains("popw") || u.contains("npcmax") || u.contains("maxnpc"))) continue;
            StringBuilder refs = new StringBuilder();
            for (Reference r : getReferencesTo(d.getAddress())) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f == null) continue;
                refs.append(" ").append(f.getName());
                if (f.getBody().getNumAddresses() < 12000) todo.put(f.getName(), f);
            }
            out.println(d.getAddress() + "  \"" + s + "\" " + refs);
        }
        for (long a : new long[]{0x0061f150L, 0x00599d20L, 0x0059c1d0L, 0x0059c300L}) {
            Function f = getFunctionAt(toAddr(a)); if (f != null) todo.put(f.getName(), f);
        }
        out.println();
        for (Function f : todo.values()) {
            DecompileResults dr = dec.decompileFunction(f, 240, new ConsoleTaskMonitor());
            String c = (dr != null && dr.getDecompiledFunction() != null) ? dr.getDecompiledFunction().getC() : "(failed)";
            out.println("################ " + f.getName() + " @ " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses() + " ################");
            out.println(c.length() > 30000 ? c.substring(0, 30000) + "\n...[truncated]" : c);
            Set<String> up = new TreeSet<>();
            for (Reference r2 : getReferencesTo(f.getEntryPoint())) {
                Function c2 = getFunctionContaining(r2.getFromAddress());
                if (c2 != null) up.add(c2.getName());
            }
            out.println("---- callers: " + up);
            out.println();
        }
        out.close();
        println("wrote mob_npc.txt");
    }
}
