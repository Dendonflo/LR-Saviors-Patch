import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.data.StringDataType;
import ghidra.program.model.scalar.Scalar;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;
import java.util.regex.*;

// Reconnaissance for the next graphics round (2026-09-14). Three questions,
// one run:
//
// 1. DoF / post pyramid. FUN_00b00c00 (post pyramid allocator) sizes the
//    whole bloom/DoF chain from settings [0511558c]+0x1c, and the change
//    detector FUN_00b014b0 rebuilds set 0 when slot 0 no longer matches
//    +0x18/+0x1c - the same mechanism ShadowMapRes and SsaaScale ride on.
//    Before writing that field: who WRITES it (the 1280x720 initialiser) and
//    who else READS it (if the UI layout uses it as its design resolution,
//    scaling it breaks the HUD). Also the DepthOfField/Glare/Lighting menu
//    handlers, found by name the way Shadowing's were.
//
// 2. Shadow filtering. FUN_00ac6b00 (MS_SHADOW) hands everything to
//    FUN_00a525c0 - the screen-space shadow renderer. Decompile it and its
//    gate FUN_00b03780 to see what parameters the filter takes (radius,
//    sample count) and where the shader is selected.
//
// 3. LOD / NPC appear distance. Vocabulary scan for LOD/distance/cull/fade
//    strings with their referencing functions - the cheap first pass before
//    any runtime tracing.
public class GfxNextRecon extends GhidraScript {
    private DecompInterface dec;
    private PrintWriter out;
    private Set<String> seen = new HashSet<>();

    private String decomp(Function f, int cap) {
        DecompileResults r = dec.decompileFunction(f, 240, new ConsoleTaskMonitor());
        String c = (r != null && r.getDecompiledFunction() != null)
                   ? r.getDecompiledFunction().getC() : "(decompile failed)";
        if (c.length() > cap) c = c.substring(0, cap) + "\n... [truncated]";
        return c;
    }

    private Function dump(String addr, String why, int cap) {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        Function f = getFunctionContaining(a);
        if (f == null) { out.println("(no function at " + addr + ")"); return null; }
        if (!seen.add(f.getEntryPoint().toString())) return f;
        out.println("################ " + f.getName() + " @ " + f.getEntryPoint()
                    + "  size=" + f.getBody().getNumAddresses() + "   (" + why + ") ################");
        out.println(decomp(f, cap));
        out.println();
        return f;
    }

    // In the registration function, a string ref is followed (or preceded)
    // by PUSH <code address> - that is the handler. Scan a few instructions
    // around the ref for a scalar operand that lands inside a function.
    private String handlerNear(Address ref) {
        Listing lst = currentProgram.getListing();
        Instruction ins = lst.getInstructionContaining(ref);
        if (ins == null) return "?";
        List<Instruction> win = new ArrayList<>();
        Instruction p = ins;
        for (int i = 0; i < 4 && p != null; i++) { win.add(0, p); p = p.getPrevious(); }
        Instruction n = ins.getNext();
        for (int i = 0; i < 4 && n != null; i++) { win.add(n); n = n.getNext(); }
        StringBuilder sb = new StringBuilder();
        for (Instruction w : win) {
            for (int op = 0; op < w.getNumOperands(); op++) {
                for (Object o : w.getOpObjects(op)) {
                    long v = -1;
                    if (o instanceof Scalar) v = ((Scalar) o).getUnsignedValue();
                    else if (o instanceof Address) v = ((Address) o).getOffset();
                    if (v < 0x401000 || v > 0x1000000) continue;
                    Address a = toAddr(v);
                    Function f = getFunctionAt(a);
                    if (f != null) sb.append(" ").append(f.getName());
                }
            }
        }
        return sb.length() == 0 ? "?" : sb.toString();
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/gfx_next_recon.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();

        // ---------------- 1a. menu handlers by name ----------------
        out.println("======== 1a. Graphics_* handlers (string ref -> PUSHed function) ========");
        String[] want = { "Graphics_DepthOfField_Standard", "Graphics_DepthOfField_Advanced",
                          "Graphics_Glare_Standard", "Graphics_Glare_Advanced",
                          "Graphics_Lighting_Standard", "Graphics_Lighting_Advanced",
                          "Graphics_ColorCorrection_Standard", "Graphics_ColorCorrection_Advanced",
                          "Graphics_Shadowing_Advanced" /* control: must say FUN_00acac90 */ };
        Map<String, Set<String>> handlers = new LinkedHashMap<>();
        DataIterator di = lst.getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            if (!(d.getDataType() instanceof StringDataType)) continue;
            Object v = d.getValue(); if (v == null) continue;
            String s = v.toString();
            for (String w : want) if (s.equals(w)) {
                for (Reference r : getReferencesTo(d.getAddress())) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f == null || !f.getName().equals("FUN_00acaf60")) continue;
                    String h = handlerNear(r.getFromAddress());
                    out.printf("%-36s ref %s -> %s%n", s, r.getFromAddress(), h);
                    handlers.computeIfAbsent(s, k -> new LinkedHashSet<>());
                    for (String hn : h.trim().split(" ")) if (hn.startsWith("FUN_")) handlers.get(s).add(hn);
                }
            }
        }
        out.println();
        out.println("======== 1b. handler bodies ========");
        for (Map.Entry<String, Set<String>> e : handlers.entrySet())
            for (String hn : e.getValue())
                dump(hn.substring(4), e.getKey(), 4000);

        // ---------------- 1c. settings +0x18/+0x1c writers & readers ----------------
        out.println("======== 1c. [0511558c]+0x18 / +0x1c : every function touching them ========");
        Address settings = toAddr(0x0511558c);
        Set<Function> fs = new LinkedHashSet<>();
        for (Reference r : getReferencesTo(settings)) {
            Function f = getFunctionContaining(r.getFromAddress());
            if (f != null) fs.add(f);
        }
        Pattern rd = Pattern.compile("\\(DAT_0511558c \\+ 0x1[8c]\\)");
        Pattern wr = Pattern.compile("\\(DAT_0511558c \\+ 0x1[8c]\\) = ");
        List<String> writers = new ArrayList<>();
        List<String> readers = new ArrayList<>();
        Map<String, String> bodies = new HashMap<>();
        for (Function f : fs) {
            String c = decomp(f, 60000);
            boolean r = rd.matcher(c).find();
            boolean w = wr.matcher(c).find();
            if (w) { writers.add(f.getName()); bodies.put(f.getName(), c); }
            else if (r) readers.add(f.getName() + " (size " + f.getBody().getNumAddresses() + ")");
        }
        out.println("-- WRITERS (" + writers.size() + ") --");
        for (String w : writers) out.println("   " + w);
        out.println("-- READERS (" + readers.size() + ") --");
        for (String r : readers) out.println("   " + r);
        out.println();
        for (String w : writers) {
            out.println("################ WRITER " + w + " ################");
            String c = bodies.get(w);
            if (c.length() > 12000) c = c.substring(0, 12000) + "\n... [truncated]";
            out.println(c); out.println();
        }
        // The getters and who calls them (indirect readers).
        for (String g : new String[]{"0075f150", "0075f160", "007c0d60", "007c0d70"}) {
            Function f = getFunctionContaining(toAddr(Long.parseLong(g, 16)));
            if (f == null) continue;
            out.println("-- getter " + f.getName() + " : " + decomp(f, 600).replace("\n", " ").replaceAll("\\s+", " "));
            Set<String> callers = new TreeSet<>();
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                Function cf = getFunctionContaining(r.getFromAddress());
                if (cf != null) callers.add(cf.getName());
            }
            out.println("   callers (" + callers.size() + "): " + callers);
        }
        out.println();
        dump("00b00640", "post set (set 0) rebuild - what the change detector calls", 8000);
        dump("00b00810", "screen set rebuild (reference)", 4000);

        // ---------------- 2. screen-space shadow renderer ----------------
        out.println("======== 2. screen-space shadow renderer ========");
        dump("00b03780", "gate: full shadow path vs FUN_00a524f0 fallback", 4000);
        dump("00a524f0", "MS_SHADOW fallback (no-shadow path?)", 4000);
        Function ssr = dump("00a525c0", "screen-space shadow renderer, called from MS_SHADOW", 30000);
        if (ssr != null) {
            out.println("---- callees of FUN_00a525c0 ----");
            Set<String> callees = new TreeSet<>();
            for (Function cf : ssr.getCalledFunctions(new ConsoleTaskMonitor())) callees.add(cf.getName());
            out.println(callees);
            out.println("---- float/int immediates in FUN_00a525c0 (kernel radii, sample counts) ----");
            InstructionIterator ii = lst.getInstructions(ssr.getBody(), true);
            int n = 0;
            while (ii.hasNext() && n < 80) {
                Instruction in = ii.next();
                String m = in.getMnemonicString();
                if (!(m.startsWith("MOV") || m.startsWith("PUSH") || m.startsWith("CMP") || m.startsWith("LEA"))) continue;
                for (int op = 0; op < in.getNumOperands(); op++)
                    for (Object o : in.getOpObjects(op))
                        if (o instanceof Scalar) {
                            long v = ((Scalar) o).getUnsignedValue();
                            if (v > 1 && v < 0x400) { out.println("   " + in.getAddress() + "  " + in); n++; }
                        }
            }
        }
        dump("00ac6e50", "DRAW_MULTI_SAMPLE handler (the interleave/soften step)", 12000);
        out.println();

        // ---------------- 3. LOD / distance vocabulary ----------------
        out.println("======== 3. LOD / distance / cull / fade strings ========");
        String[] pats = { "LOD", "DISTANCE", "_DIST", "DIST_", "CULL", "FADE", "APPEAR",
                          "VISIBLE_RANGE", "VIEWRANGE", "VIEW_RANGE", "DRAWRANGE", "DRAW_RANGE",
                          "CLIP", "NEAR", "FAR_", "POPUP", "SPAWN", "NPC", "CHARA_LOD", "MODEL_LOD" };
        di = lst.getDefinedData(true);
        int hits = 0;
        while (di.hasNext()) {
            Data d = di.next();
            if (!(d.getDataType() instanceof StringDataType)) continue;
            Object v = d.getValue(); if (v == null) continue;
            String s = v.toString();
            if (s.length() < 4 || s.length() > 90) continue;
            String u = s.toUpperCase();
            boolean match = false;
            for (String p : pats) if (u.contains(p)) { match = true; break; }
            if (!match) continue;
            // skip obvious noise: file extensions / paths / format strings only
            if (u.contains("%") && u.length() < 8) continue;
            StringBuilder refs = new StringBuilder();
            int n = 0;
            for (Reference r : getReferencesTo(d.getAddress())) {
                if (n++ > 3) { refs.append(" ..."); break; }
                Function f = getFunctionContaining(r.getFromAddress());
                refs.append("  ").append(f == null ? r.getFromAddress().toString() : f.getName());
            }
            out.printf("%-10s %-60s%s%n", d.getAddress(), "\"" + s + "\"",
                       refs.length() == 0 ? "  (no refs)" : refs.toString());
            hits++;
        }
        out.println("(" + hits + " matches)");
        out.close();
        println("wrote gfx_next_recon.txt");
    }
}
