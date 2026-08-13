import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// DAT_0234f130 is the global frame/time context: [+0x4c] is used as a delta in
// the per-frame field update, [+0x54] is the "now" that gates the chocobo-girl
// timer in FUN_005b4fb0 (plays "system_choco1", first call of the frame).
//
// Whether [+0x54] is a millisecond clock or a frame counter decides whether
// every deadline expressed in it is frame-rate dependent. Find its writer.
public class FrameClock extends GhidraScript {

    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/frame_clock.txt","UTF-8");

        // The pointer global itself
        Address g = currentProgram.getAddressFactory().getAddress("0234f130");
        out.println("######## refs to DAT_0234f130 ########");
        Map<String,Integer> fns = new LinkedHashMap<>();
        for (Reference r : getReferencesTo(g)) {
            Function f = getFunctionContaining(r.getFromAddress());
            String k = f == null ? "(none)" : f.getName() + " @ " + f.getEntryPoint();
            fns.merge(k, 1, Integer::sum);
        }
        for (Map.Entry<String,Integer> e : fns.entrySet())
            out.println("  " + e.getKey() + "  x" + e.getValue());

        // Every instruction that WRITES [reg+0x4c] or [reg+0x54] anywhere, where
        // the write looks like a clock update (INC / ADD / MOV from a computed
        // value). Report with enclosing function so the tick can be identified.
        out.println("\n######## writes to [reg+0x4c] / [reg+0x54] ########");
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext()) {
            Instruction i = it.next();
            String s = i.toString();
            boolean off = s.contains("0x4c]") || s.contains("0x54]");
            if (!off) continue;
            String m = i.getMnemonicString();
            if (!m.equals("INC") && !m.equals("ADD") && !m.equals("MOV")) continue;
            // destination must be the memory operand (a store)
            if (!s.matches("^(INC|ADD|MOV)\\s+(dword ptr )?\\[.*")) continue;
            Function f = getFunctionContaining(i.getAddress());
            out.println("  " + i.getAddress() + "  " + s
                        + (f == null ? "" : "   in " + f.getName() + " @ " + f.getEntryPoint()));
        }

        // Decompile the chocobo-girl timer's neighbours and whatever writes the
        // clock, to read the units directly.
        out.println("\n######## FUN_005b24c0 (reads [ESI+0x10]) ########");
        DecompInterface d = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.setMaxPayloadMBytes(128);
        d.setOptions(opts);
        d.openProgram(currentProgram);
        for (String t : new String[]{ "005b24c0" }) {
            Address a = currentProgram.getAddressFactory().getAddress(t);
            Function f = getFunctionContaining(a);
            if (f == null) continue;
            DecompileResults dr = d.decompileFunction(f, 300, new ConsoleTaskMonitor());
            out.println(dr != null && dr.decompileCompleted()
                        ? dr.getDecompiledFunction().getC() : "FAILED");
        }
        d.dispose();
        out.close();
        println("DONE");
    }
}
