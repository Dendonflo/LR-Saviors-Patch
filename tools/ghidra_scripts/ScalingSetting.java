import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// AA is CONFIRMED present with MSAA off, on geometry AND alpha-tested
// cutouts, at native res, and it survives replacing every fullscreen pixel
// shader with a passthrough. So it is not a post-process draw. That leaves an
// engine-side image mode - and the settings table exposes exactly one
// candidate family: "Graphics_Scaling" with values None / Standard /
// Advanced (aa_settings.txt lines 39, 56-58), handled by FUN_00acaf60, the
// same name->setting mapper that yielded Graphics_Shadowing (which is how
// ShadowMapRes was found and fixed).
//
// Goal: find WHERE the scaling value lands in the settings object
// (DAT_0511558c) and WHO reads it, so it can be written the same way
// ShadowMapRes is - no D3D9 hook, no shader replacement.
public class ScalingSetting extends GhidraScript {
    private DecompInterface dec;
    private PrintWriter out;

    private void dump(Address a, String why) {
        Function f = getFunctionContaining(a);
        if (f == null) { out.println("(no function at " + a + " - " + why + ")"); return; }
        out.println("################ " + f.getName() + " @ " + f.getEntryPoint()
                    + "   (" + why + ") ################");
        DecompileResults r = dec.decompileFunction(f, 240, new ConsoleTaskMonitor());
        String c = (r != null && r.getDecompiledFunction() != null)
                   ? r.getDecompiledFunction().getC() : "(decompile failed)";
        if (c.length() > 24000) c = c.substring(0, 24000) + "\n... [truncated]";
        out.println(c);
        out.println();
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/scaling_setting.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        // 1. The settings name mapper itself - shows the ID each Graphics_*
        //    name maps to, which is the index into the settings object.
        dump(currentProgram.getAddressFactory().getAddress("00acaf60"),
             "Graphics_* name -> setting id mapper");

        // 2. Everything that references the Graphics_Scaling strings.
        String[] targets = { "0218b3cc", "02198c84", "02198c9c", "02198cb8" };
        String[] names = { "Graphics_Scaling", "..._None", "..._Standard", "..._Advanced" };
        for (int i = 0; i < targets.length; i++) {
            Address a = currentProgram.getAddressFactory().getAddress(targets[i]);
            out.println("======== references to " + names[i] + " (" + targets[i] + ") ========");
            for (Reference r : getReferencesTo(a)) {
                Function f = getFunctionContaining(r.getFromAddress());
                out.println("   " + r.getFromAddress()
                            + (f == null ? "" : "  in " + f.getName()));
            }
            out.println();
        }

        // 3. The settings object. Every function touching DAT_0511558c is a
        //    candidate consumer; the DRAW_FILTER handler already gates on
        //    +0x2c/+0x2d, so neighbouring offsets are where image-mode flags
        //    live. List the referencing functions with the offsets they use.
        Address settings = currentProgram.getAddressFactory().getAddress("0511558c");
        out.println("======== functions referencing the settings object 0511558c ========");
        Set<Function> consumers = new LinkedHashSet<>();
        for (Reference r : getReferencesTo(settings)) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.println("   " + r.getFromAddress() + (f == null ? "" : "  in " + f.getName()));
            if (f != null) consumers.add(f);
        }
        out.println("(" + consumers.size() + " distinct functions)");
        out.println();

        // 4. Decompile the smaller consumers - a setting READER is typically
        //    tiny (load a field, compare, branch), exactly like the
        //    Graphics_Shadowing handlers turned out to be.
        int dumped = 0;
        for (Function f : consumers) {
            long size = f.getBody().getNumAddresses();
            if (size > 1200) continue;
            dump(f.getEntryPoint(), "settings consumer, size=" + size);
            if (++dumped >= 25) break;
        }
        out.close();
        println("wrote scaling_setting.txt");
    }
}
