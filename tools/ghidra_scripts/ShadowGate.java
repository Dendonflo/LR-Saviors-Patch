import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// The game's own shadow quality menu only offers 1024 and 2048 - there is no
// "off". But the DRAW_SHADOW pass handler FUN_00ac6040 is gated on fields of
// the settings object at *(0x0511558c), which ApplyShadowResolution already
// writes to (offset +0x24 = resolution). If the gate is a plain enable flag,
// writing it is a clean way to disable the pass entirely - same philosophy as
// every other intervention here: write the value the engine reads rather than
// patch its instructions.
//
// Need to know exactly: which offsets, what width, and what the test is.
public class ShadowGate extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/shadow_gate.txt","UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();

        Address a = currentProgram.getAddressFactory().getAddress("00ac6040");
        Function f = getFunctionContaining(a);
        out.println("======== FUN_00ac6040 (DRAW_SHADOW handler) ========");
        DecompileResults r = dec.decompileFunction(f, 120, new ConsoleTaskMonitor());
        out.println(r != null && r.getDecompiledFunction() != null
                    ? r.getDecompiledFunction().getC() : "(decompile failed)");

        out.println("---- first 40 instructions (to see operand widths) ----");
        Instruction i = lst.getInstructionAt(f.getEntryPoint());
        for (int n = 0; n < 40 && i != null; n++) {
            out.printf("  %s  %s%n", i.getAddress(), i.toString());
            i = i.getNext();
        }
        out.close();
        println("wrote shadow_gate.txt");
    }
}
