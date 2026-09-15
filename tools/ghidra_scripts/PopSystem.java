import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// NPC population ("POP") and load ("LD") systems. The FieldGlobal parameter
// getters are FUN_005b2f20 (float, idx*0x20 + 0x024c8a44) and FUN_005b2f40
// (int). Decompile every function that calls either getter, print it with
// the getter calls annotated by parameter NAME (index -> name from the
// static table: name at 0x024c8a30 + idx*0x20, walked from the exe's own
// initialiser order which matches the loader's), so the pop/depop/budget
// logic can be read with its inputs named. Names are read from memory in
// the Ghidra program (the initialiser copies them in at runtime, so the
// static .data is empty) - instead the index->name map is passed in below,
// derived offline from the initialiser (tools work, 2026-09-14).
public class PopSystem extends GhidraScript {
    private static final String[] NAMES = new String[112];
    static {
        // idx -> name, from the offline initialiser walk (patch of 2026-09-14)
        String[][] known = {{"0","RegaHpPerSec"},{"1","RegaAtbPerSec"},{"2","PRegaAtbPerSec"},{"3","PEndAtb"},{"4","LAtbDashPerSec"},{"5","LAtbAttack"},{"6","SRegaAtbAttack"},{"7","LifeDecSpeDeath"},{"8","LDBleRatio"},{"9","LDHandSpeed"},{"10","LDHandSpeedMax"},{"11","LDOverSpeedMul"},{"12","LDSub0Speed"},{"13","LDSub0Timer"},{"14","LDSub0Border"},{"15","LDSub1Speed"},{"16","LDSub1Timer"},{"17","LDSub1Border"},{"18","LDSub2Speed"},{"19","LDSub2Timer"},{"20","LDAdd0Speed"},{"21","LDAdd0Timer"},{"22","LDAdd0Border"},{"23","LDAdd1Speed"},{"24","LDAdd1Timer"},{"25","LDAdd1Border"},{"26","LDAdd2Speed"},{"27","LDAdd2Timer"},{"28","LDModLevel1"},{"29","LDModLevel2"},{"30","LDModLevel3"},{"31","LDPriority"},{"32","LDUconTimer"},{"33","POPWNearLength"},{"34","POPWPriority"},{"35","POPWLoadRes"},{"36","POPSuggBordar"},{"37","POPPopLength"},{"38","POPDepopLength"},{"39","POPWNearLenMob"},{"40","DZDengerLength"},{"41","ATKToAggTime0"},{"42","ATKToAtkTime0"},{"43","ATKToAggTime1"},{"44","ATKToAtkTime1"},{"45","ATKToAggTime2"},{"46","ATKDirTime1"},{"47","ATKDirRatio1"},{"48","ATKDirTime2"},{"49","ATKDirRatio2"},{"50","LAtbAttack2"},{"51","LAtbAttack3"},{"52","DGToAggTime"},{"53","LAtbDodge"},{"54","FEBackAngle"},{"55","FESideAngle"},{"56","TRSeStopTime"},{"57","TRSeLoopFadeSpd"},{"58","TRSeArrVoiTime"},{"59","TRSeArrAlmTime"},{"60","FEPopRange"},{"61","ATKDirTime0"},{"62","ATKDirRatio0"},{"63","BGMFadeTime"},{"64","ChocoAtbLoss"},{"65","BKShelfCapacity"},{"66","FEEffExcellent"},{"67","FEEffGood"},{"68","FEEffBad"},{"69","FEViewDegFar"},{"70","FEViewRangeFar"},{"71","FEViewDegNear"},{"72","FEViewRangeNear"},{"73","FEFindFrame"},{"74","FEOutViewFrame"},{"75","JumpPortalHour"},{"76","FEMovEndTime"},{"77","FEMovEndTimeRan"},{"78","FEMovTime"},{"79","FEMovTimeRan"},{"80","FEPcViewDegFar"},{"81","FEPcViewRanFar"},{"82","FEPcViewDegNear"},{"83","FEPcViewRanNear"},{"84","FEPopRangeDeath"},{"85","ChoGlideBsGra"},{"86","ChoGlideUpTime"},{"87","ChoGlideUpGrav"},{"88","ChoGlideUpUdRt"},{"89","ChoHelpLength"},{"90","DZDepopLength"},{"91","BGMBtFieldFO"},{"92","BGMBtBattleFI"},{"93","BGMFrBattleFO"},{"94","BGMFrFanfareFI"},{"95","BGMFrFanfareFO"},{"96","BGMFrFieldFI"},{"97","BGMOCBattleFO"},{"98","BGMOCOverCFI"},{"99","BGMOCOverCFO"},{"100","BGMOCBattleFI"},{"101","BGMDominantBtFO"},{"102","BGMDominantDmFI"},{"103","BGMChocoboTime"},{"104","POPMobBase"},{"105","FEPopRotRand"},{"106","TRVisibleLength"},{"107","FEPopTimerBase"},{"108","FEPopTimerRand"},{"109","TRSparkLenBase"},{"110","TRSparkLenRand"}};
        for (String[] k : known) NAMES[Integer.parseInt(k[0])] = k[1];
    }

    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/pop_system.txt", "UTF-8");
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        Function getF = getFunctionAt(toAddr(0x005b2f20));
        Function getI = getFunctionAt(toAddr(0x005b2f40));
        Map<String, Function> callers = new TreeMap<>();
        for (Function g : new Function[]{getF, getI})
            for (Reference r : getReferencesTo(g.getEntryPoint())) {
                Function cf = getFunctionContaining(r.getFromAddress());
                if (cf != null) callers.put(cf.getEntryPoint().toString(), cf);
            }
        out.println("getter callers: " + callers.size());
        for (Function cf : callers.values()) {
            DecompileResults dr = dec.decompileFunction(cf, 240, new ConsoleTaskMonitor());
            String c = (dr != null && dr.getDecompiledFunction() != null) ? dr.getDecompiledFunction().getC() : "(failed)";
            // annotate FUN_005b2f20(N) / FUN_005b2f40(N) with names
            StringBuffer sb = new StringBuffer();
            java.util.regex.Matcher m = java.util.regex.Pattern.compile("FUN_005b2f([24])0\\((0x[0-9a-f]+|\\d+)\\)").matcher(c);
            while (m.find()) {
                String num = m.group(2);
                int idx = num.startsWith("0x") ? Integer.parseInt(num.substring(2), 16) : Integer.parseInt(num);
                String nm = (idx >= 0 && idx < NAMES.length && NAMES[idx] != null) ? NAMES[idx] : ("idx" + idx);
                m.appendReplacement(sb, "FieldGlobal_" + (m.group(1).equals("2") ? "F" : "I") + "(" + nm + ")");
            }
            m.appendTail(sb);
            out.println("################ " + cf.getName() + " @ " + cf.getEntryPoint() + " size=" + cf.getBody().getNumAddresses() + " ################");
            String s = sb.toString();
            out.println(s.length() > 40000 ? s.substring(0, 40000) + "\n...[truncated]" : s);
            Set<String> up = new TreeSet<>();
            for (Reference r2 : getReferencesTo(cf.getEntryPoint())) {
                Function c2 = getFunctionContaining(r2.getFromAddress());
                if (c2 != null) up.add(c2.getName());
            }
            out.println("---- callers: " + up);
            out.println();
        }
        out.close();
        println("wrote pop_system.txt");
    }
}
