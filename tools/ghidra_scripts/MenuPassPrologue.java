import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// Prologue bytes for the two remaining draw-pass handlers, so the SSAA probe
// can pick a patchLen that does not cut an instruction in half (the existing
// pass hooks use 5/6/9 for exactly this reason, and a mis-sized patch
// corrupts the trampoline rather than failing loudly):
//   DRAW_MENU        FUN_00ac6150
//   DRAW_BACK_BUFFER FUN_00ab7810
// Also decompiles both - DRAW_BACK_BUFFER is the suspected home of the final
// StretchRect, whose filter argument decides whether a supersampled scene
// would actually be downsampled or just point-decimated.
public class MenuPassPrologue extends GhidraScript {
    private PrintWriter out;
    private DecompInterface dec;

    private void show(String addr, String why) {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        Function f = getFunctionContaining(a);
        out.println("################ " + addr + "  (" + why + ") ################");
        if (f == null) { out.println("(no function)"); return; }

        Listing lst = currentProgram.getListing();
        Instruction ins = lst.getInstructionAt(a);
        int cum = 0;
        out.println("-- prologue, with cumulative byte offsets (pick a patchLen that lands on a boundary) --");
        for (int i = 0; i < 12 && ins != null; i++) {
            StringBuilder hex = new StringBuilder();
            try {
                for (byte b : ins.getBytes()) hex.append(String.format("%02X ", b));
            } catch (Exception e) { hex.append("??"); }
            cum += ins.getLength();
            out.printf("   %s  %-24s %-34s  ends at +%d%n",
                       ins.getAddress(), hex.toString(), ins.toString(), cum);
            ins = ins.getNext();
        }
        out.println();
        DecompileResults r = dec.decompileFunction(f, 240, new ConsoleTaskMonitor());
        String c = (r != null && r.getDecompiledFunction() != null)
                   ? r.getDecompiledFunction().getC() : "(decompile failed)";
        if (c.length() > 20000) c = c.substring(0, 20000) + "\n... [truncated]";
        out.println(c);
        out.println();
    }

    @Override public void run() throws Exception {
        out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/menu_pass_prologue.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        show("00ac6150", "DRAW_MENU handler");
        show("00ab7810", "DRAW_BACK_BUFFER handler");
        out.close();
        println("wrote menu_pass_prologue.txt");
    }
}
