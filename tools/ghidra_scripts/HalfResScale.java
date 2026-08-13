import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// FUN_00aa3960 creates the half-res screen-space shadow targets. Its call site
// applies FUN_00aa3010 to BOTH dimensions with a shared second argument before
// calling CreateTexture:
//
//   MOV EAX,[ESI + 0x14]   ; width
//   CALL FUN_00aa3010      ; transform(width, EDI)
//   MOV ECX,[ESI + 0x10]   ; height
//   CALL FUN_00aa3010      ; transform(height, EDI)
//   CALL [EBX + 0x5c]      ; CreateTexture
//
// So FUN_00aa3010 is the size transform and the halving lives in it or in the
// scale code it is handed. FUN_00aa3ce0 is the sole caller of FUN_00aa3960 and
// is where the descriptor (ESI) gets its base dimensions.
//
// Question that decides the intervention: is the divisor a value read from
// memory (writeable, the ShadowMapRes pattern) or a literal shift (needs an
// instruction patch)?
public class HalfResScale extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/halfres_scale.txt","UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();

        String[][] fns = {
            { "00aa3010", "size transform applied to BOTH dimensions" },
            { "00aa3ce0", "sole caller of FUN_00aa3960 - fills the descriptor" },
        };
        for (String[] p : fns) {
            Address a = currentProgram.getAddressFactory().getAddress(p[0]);
            Function f = getFunctionContaining(a);
            out.println("################ FUN_" + p[0] + "  (" + p[1] + ") ################");
            if (f == null) { out.println("  no function"); continue; }
            out.println("size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 180, new ConsoleTaskMonitor());
            String c = (r != null && r.getDecompiledFunction() != null)
                       ? r.getDecompiledFunction().getC() : "(failed)";
            if (c.length() > 10000) c = c.substring(0, 10000) + "\n... [truncated]";
            out.println(c);

            out.println("---- full disassembly ----");
            Instruction i = lst.getInstructionAt(f.getEntryPoint());
            for (int n = 0; n < 70 && i != null; n++) {
                out.printf("  %s  %s%n", i.getAddress(), i.toString());
                i = i.getNext();
            }
            out.println();
        }
        out.close();
        println("wrote halfres_scale.txt");
    }
}
