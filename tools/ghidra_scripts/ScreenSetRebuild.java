import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// FUN_00b00810 is the screen-SET rebuild, called by the change detector
// FUN_00b014b0 when slot 0x21's recorded dimensions disagree with the settings
// screen size. Descriptor-mode SSAA scales those dimensions, so the
// disagreement is permanent and the rebuild runs every frame (measured:
// descScaled climbing ~390 per report interval, 64k+ in one session).
//
// The fix gates this call. Needed here: the prologue with cumulative byte
// offsets so the JMP patch lands on an instruction boundary, and the
// decompile to confirm it takes no argument that the gate would strand.
public class ScreenSetRebuild extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/screenset_rebuild.txt", "UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        String[] targets = { "00b00810", "00b00940", "00b00640" };
        String[] why = { "screen-set rebuild (the one to gate)",
                         "shadow-map rebuild", "post-pyramid rebuild" };
        for (int i = 0; i < targets.length; i++) {
            Address a = currentProgram.getAddressFactory().getAddress(targets[i]);
            out.println("################ " + targets[i] + "  (" + why[i] + ") ################");
            Instruction ins = currentProgram.getListing().getInstructionAt(a);
            int cum = 0;
            for (int k = 0; k < 10 && ins != null; k++) {
                StringBuilder hex = new StringBuilder();
                try { for (byte b : ins.getBytes()) hex.append(String.format("%02X ", b)); }
                catch (Exception e) { hex.append("??"); }
                cum += ins.getLength();
                out.printf("   %s  %-22s %-32s ends at +%d%n",
                           ins.getAddress(), hex.toString(), ins.toString(), cum);
                ins = ins.getNext();
            }
            Function f = getFunctionContaining(a);
            if (f != null) {
                DecompileResults r = dec.decompileFunction(f, 180, new ConsoleTaskMonitor());
                String c = (r != null && r.getDecompiledFunction() != null)
                           ? r.getDecompiledFunction().getC() : "(decompile failed)";
                if (c.length() > 9000) c = c.substring(0, 9000) + "\n... [truncated]";
                out.println(c);
            }
            out.println();
        }
        out.close();
        println("wrote screenset_rebuild.txt");
    }
}
