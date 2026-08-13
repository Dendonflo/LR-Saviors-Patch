import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// Runtime probe on the TextureImp constructor named the caller that fills the
// descriptor for the half-res screen-space shadow targets:
//
//   [halfres] desc 1920x1080 (w@+0x0C h@+0x10) decided by 00A94845
//   raw: 00000000 00000001 00000000 00000780 00000438 00000001 00000001 0000000{9,4} ...
//
// So the descriptor is { ?, 1, ?, width, height, 1, 1, format, 0, ... } and
// 00A94845 is the return address of the call that consumed it - i.e. the /2
// happens in the function containing it, just before.
//
// What decides the intervention:
//   - divisor read from a settings field  -> write it (the ShadowMapRes shape,
//     the only shadow intervention on this engine that has actually worked)
//   - literal SHR 1 / IDIV 2              -> instruction patch, more invasive
//   - and critically: WHO ELSE reads these dimensions? ShadowScale failed
//     because the surface grew while its consumers kept the old size.
public class HalfResDecider extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/halfres_decider.txt","UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();

        Address a = currentProgram.getAddressFactory().getAddress("00a94845");
        Function f = getFunctionContaining(a);
        if (f == null) { out.println("no function containing 00a94845"); out.close(); return; }
        out.println("################ " + f.getName() + " @ " + f.getEntryPoint()
                    + " size=" + f.getBody().getNumAddresses() + " ################");
        DecompileResults r = dec.decompileFunction(f, 180, new ConsoleTaskMonitor());
        String c = (r != null && r.getDecompiledFunction() != null)
                   ? r.getDecompiledFunction().getC() : "(decompile failed)";
        if (c.length() > 16000) c = c.substring(0, 16000) + "\n... [truncated]";
        out.println(c);

        out.println("---- instructions around 00a94845 ----");
        Instruction i = lst.getInstructionAt(a);
        for (int n = 0; n < 40 && i != null; n++) i = i.getPrevious();
        for (int n = 0; n < 75 && i != null; n++) {
            out.printf("  %s  %-36s%s%n", i.getAddress(), i.toString(),
                       i.getAddress().equals(a) ? "  <== return addr" : "");
            i = i.getNext();
        }

        out.println();
        out.println("---- every shift / divide in this function ----");
        InstructionIterator it = lst.getInstructions(f.getBody(), true);
        while (it.hasNext()) {
            Instruction x = it.next();
            String t = x.toString();
            if (t.startsWith("SHR") || t.startsWith("SAR") || t.startsWith("IDIV")
                || t.startsWith("DIV") || t.startsWith("IMUL") || t.startsWith("MUL"))
                out.println("    " + x.getAddress() + "  " + t);
        }

        out.println();
        out.println("---- callers of " + f.getName() + " ----");
        for (Reference rf : getReferencesTo(f.getEntryPoint())) {
            Function c2 = getFunctionContaining(rf.getFromAddress());
            out.println("  from " + rf.getFromAddress()
                        + (c2 == null ? "" : "  in " + c2.getName() + " @ " + c2.getEntryPoint()));
        }
        out.close();
        println("wrote halfres_decider.txt");
    }
}
