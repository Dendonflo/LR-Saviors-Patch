import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// Identify the addresses that dominated the 2026-08-13 20ms-threshold stutter
// capture. Two families showed up in the >40ms frames:
//
//   009FD156  - sampled as EIP three times, ALWAYS with an empty EBP chain
//               (FPO leaf) and always with 009DExxx/009DFxxx below it on the
//               raw stack. Prime suspect for the "linear scan over the whole
//               registered-native-method table per class member" already
//               described in PROGRESS.md's Cause 3.
//   009DE0CB  - sits inside FUN_009ddfe0, the recursive nested-class resolver
//               named in that same section. Its EBP chain (009DF3E7 009DFF8D
//               009A2805 ...) matches the documented Cause-3 stack almost
//               exactly, which is why this run is being treated as a
//               confirmation of that cause rather than a new one.
//
// What is needed here: the containing function for each address (so the log's
// bare VAs stop being anonymous), the decompile of the two hot ones to see
// whether the loop really is a linear table scan, and the call graph in/out so
// a throttle point can be chosen with the callers visible.
public class SpikeStacks extends GhidraScript {

    private PrintWriter out;
    private DecompInterface dec;

    private Function head(String tag, String vaStr) {
        Address a = currentProgram.getAddressFactory().getAddress(vaStr);
        Function f = getFunctionContaining(a);
        out.println("################ " + vaStr + "  (" + tag + ") ################");
        if (f == null) { out.println("   NO FUNCTION CONTAINING THIS ADDRESS"); return null; }
        out.printf("   in %s @ %s  (+0x%X into it), body %s%n",
                   f.getName(), f.getEntryPoint(),
                   a.getOffset() - f.getEntryPoint().getOffset(),
                   f.getBody().getNumAddresses() + " bytes");
        return f;
    }

    private void callers(Function f) {
        out.println("   -- callers --");
        int n = 0;
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            if (!r.getReferenceType().isCall()) continue;
            Function c = getFunctionContaining(r.getFromAddress());
            out.printf("      %s  in %s%n", r.getFromAddress(),
                       c != null ? c.getName() : "(none)");
            if (++n >= 12) { out.println("      ... more"); break; }
        }
        if (n == 0) out.println("      (none - indirect/vtable target)");
    }

    private void body(Function f, int limit) {
        DecompileResults r = dec.decompileFunction(f, 180, new ConsoleTaskMonitor());
        String c = (r != null && r.getDecompiledFunction() != null)
                   ? r.getDecompiledFunction().getC() : "(decompile failed)";
        if (c.length() > limit) c = c.substring(0, limit) + "\n... [truncated]";
        out.println(c);
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/spike_stacks.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        // The two hot ones: full decompile + callers.
        String[] hot    = { "00b46dc3", "00b47ad9" };
        String[] hotWhy = { "cluster B: caller of the CRT hot loop, 16 identical stacks",
                            "cluster B: its caller, under FUN_00a01a00" };
        for (int i = 0; i < hot.length; i++) {
            Function f = head(hotWhy[i], hot[i]);
            if (f == null) continue;
            callers(f);
            body(f, 12000);
            out.println();
        }

        // Everything else that appeared on a >20ms stack, named only. Enough to
        // turn the raw scan/ebp columns into a readable call path without
        // decompiling half the binary.
        String[] ctx = {
            "009fd004", "009fd1a0", "009fd26e",      // the FPO leaf's neighbourhood
            "009de484", "009df383", "009dff8d",      // loader frames seen in scan
            "009df3e7", "009a2805", "009a291e", "009a29b4",
            "00c449ef", "00c357d4", "00c35848",      // the 1137-alloc / 4.6MB-read frame
            "00d5f8e8",                              // sleep caller #2 (120k samples)
            "00ac31c8",                              // sleep caller #1 (known: limiter)
            "00a97197", "00abccf7", "00ab94be",      // seen under the AMD driver waits
            "00a01b4a", "00ab9082", "00ab97c9", "00ab974b",   // cluster B main-loop path
            "00d12992", "00d12967", "00d13098", "00cf1ff5",   // window proc / pump
            "00b4609e", "00b31b49", "00a2e787", "00a01639",   // one-off small-stutter EIPs
            "0093ba86", "00420ed4", "00ac4fc4", "00c27c07",
        };
        out.println("################ context addresses ################");
        for (String s : ctx) {
            Address a = currentProgram.getAddressFactory().getAddress(s);
            Function f = getFunctionContaining(a);
            if (f == null) { out.printf("   %s -> (no function)%n", s); continue; }
            out.printf("   %s -> %s+0x%X%n", s, f.getName(),
                       a.getOffset() - f.getEntryPoint().getOffset());
        }

        out.close();
        println("wrote spike_stacks.txt");
    }
}
