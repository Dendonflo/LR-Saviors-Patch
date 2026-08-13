import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// FrameworkDrawManager's registration gives the handler for every named pass:
//   DRAW_SHADOW                   -> FUN_00ac6040
//   DRAW_MULTI_SAMPLE_SCHEDULE    -> FUN_00ac5ee0
//   DRAW_MULTI_SAMPLE_PROPAGATION -> FUN_00ac5f50
//   DRAW_MULTI_SAMPLE_DEPTH       -> FUN_00ac6890
//   DRAW_MULTI_SAMPLE_SHADOW      -> FUN_00ac6b00
//   DRAW_MULTI_SAMPLE             -> FUN_00ac6e50
//   DRAW_FILTER                   -> FUN_00ac7030
//
// Two questions:
//   SCHEDULE/PROPAGATION - is there a rotating sample index? A 3x3 interleaved
//     grid would be 9 samples and would explain the unexplained 9-frame
//     frametime period. Looking for a counter, a modulo, or a table of offsets.
//     (Treat 3x3=9 as a GUESS until a literal count is actually found - this
//     project has twice been burned by a ratio that merely landed near a round
//     number.)
//   MULTI_SAMPLE_SHADOW - what resolution does it render at? The RT inventory
//     has 1920x1080 targets at 3840x2160 output, and the claim is that shadows
//     are applied at half res.
public class MsHandlers extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/ms_handlers.txt","UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();

        String[][] fns = {
            { "00ac5ee0", "DRAW_MULTI_SAMPLE_SCHEDULE" },
            { "00ac5f50", "DRAW_MULTI_SAMPLE_PROPAGATION" },
            { "00ac6b00", "DRAW_MULTI_SAMPLE_SHADOW" },
        };
        for (String[] pair : fns) {
            Address a = currentProgram.getAddressFactory().getAddress(pair[0]);
            Function f = getFunctionContaining(a);
            out.println("################ " + pair[1] + "  FUN_" + pair[0] + " ################");
            if (f == null) { out.println("  no function"); continue; }
            out.println("size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 120, new ConsoleTaskMonitor());
            String c = (r != null && r.getDecompiledFunction() != null)
                       ? r.getDecompiledFunction().getC() : "(failed)";
            if (c.length() > 7000) c = c.substring(0, 7000) + "\n... [truncated]";
            out.println(c);

            out.println("---- immediates (sample counts / divisors / masks) ----");
            InstructionIterator it = lst.getInstructions(f.getBody(), true);
            while (it.hasNext()) {
                Instruction i = it.next();
                String t = i.toString();
                if (t.startsWith("CMP") || t.startsWith("AND") || t.startsWith("SHR")
                    || t.startsWith("SAR") || t.startsWith("IMUL") || t.startsWith("IDIV")
                    || t.startsWith("MOD") || t.startsWith("INC") || t.startsWith("XOR"))
                    out.println("    " + i.getAddress() + "  " + t);
            }
            out.println();
        }
        out.close();
        println("wrote ms_handlers.txt");
    }
}
