import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Find the SOURCE of the half-res screen-space shadow buffer size, so it can
// be patched once (the ShadowMapRes pattern) instead of compensated at every
// consumer (the ShadowScale anti-pattern, which has now failed twice - the
// texture scale broke the viewport, the viewport scale broke the unscaled
// targets).
//
// Narrowed by the runtime probes to a short list:
//   - the dims arrive at texture factory FUN_00a94770 ALREADY halved
//   - its callers: FUN_00b00b90 FUN_00a951f0 FUN_00aa2a30 FUN_007bb7c0 FUN_00d72400
//   - FUN_00b01da0 writes DAT_05115724, the multi-sample stage object whose
//     +0x70..+0x94 slots hold the surfaces the shadow pass binds
//
// Looking for: SHR 1 / IDIV 2 / *0.5 applied to presentation dimensions, and
// WHERE the result is stored. If it lands in a stage-object field that
// creation AND the viewport both read, writing that field is the whole fix.
public class HalfResSource extends GhidraScript {

    private DecompInterface dec;
    private Listing lst;
    private PrintWriter out;

    private void dump(String addr, String why) {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        Function f = getFunctionContaining(a);
        out.println("################ FUN_" + addr + "  (" + why + ") ################");
        if (f == null) { out.println("  no function"); return; }
        out.println("entry=" + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses());
        DecompileResults r = dec.decompileFunction(f, 180, new ConsoleTaskMonitor());
        String c = (r != null && r.getDecompiledFunction() != null)
                   ? r.getDecompiledFunction().getC() : "(failed)";
        if (c.length() > 14000) c = c.substring(0, 14000) + "\n... [truncated]";
        out.println(c);

        out.println("---- shifts / divides / halving candidates ----");
        InstructionIterator it = lst.getInstructions(f.getBody(), true);
        while (it.hasNext()) {
            Instruction i = it.next();
            String t = i.toString();
            if (t.startsWith("SHR") || t.startsWith("SAR") || t.startsWith("IDIV")
                || t.startsWith("DIV"))
                out.println("    " + i.getAddress() + "  " + t);
        }
        out.println("---- callers ----");
        int n = 0;
        for (Reference rf : getReferencesTo(f.getEntryPoint())) {
            if (n++ > 8) { out.println("  ..."); break; }
            Function c2 = getFunctionContaining(rf.getFromAddress());
            out.println("  from " + rf.getFromAddress()
                        + (c2 == null ? "" : "  in " + c2.getName()));
        }
        out.println();
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/halfres_source.txt","UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        lst = currentProgram.getListing();

        dump("00b01da0", "writes DAT_05115724 - multi-sample stage object owner");
        dump("00b00b90", "caller 1 of texture factory; known shadow allocator helper");
        dump("00a951f0", "caller 2 of texture factory");
        dump("00aa2a30", "caller 3 of texture factory");
        dump("007bb7c0", "caller 4 of texture factory");
        dump("00d72400", "caller 5 of texture factory");
        out.close();
        println("wrote halfres_source.txt");
    }
}
