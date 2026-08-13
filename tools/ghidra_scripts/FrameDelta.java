import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Third-party report to verify: the SIMULATION DELTA alternates 0 / 16.683ms
// above 59.94fps. Claim is that the delta function at 0x00ac33b0 uses a
// 300000 ticks/sec clock and quantises the delta to 5005-tick steps.
//
// 5005 / 300000 = 16.6833ms = exactly one 59.94Hz frame. So the quantum IS
// the 59.94 frame, and a real 60.00fps frame (16.667ms = 5000 ticks) is just
// BELOW one quantum - which is the shape that would produce an alternating
// 0 / one-full-step output.
//
// This dumps:
//   1. the delta function and the limiter next to it, decompiled
//   2. their raw disassembly, so the rounding is visible as instructions
//      rather than through the decompiler's reinterpretation
//   3. every reference to the constants 300000 (0x493e0) and 5005 (0x138d)
//      anywhere in the binary, since the divisor/quantum may be shared
//   4. callers of the delta function - i.e. who consumes the value
public class FrameDelta extends GhidraScript {

    private DecompInterface dec;

    private String decompile(Function f) {
        if (f == null) return "(null function)";
        DecompileResults r = dec.decompileFunction(f, 120, new ConsoleTaskMonitor());
        if (r == null || r.getDecompiledFunction() == null)
            return "(decompile failed for " + f.getName() + ")";
        return r.getDecompiledFunction().getC();
    }

    private void dumpAsm(PrintWriter out, Address start, int count) {
        Listing lst = currentProgram.getListing();
        Instruction i = lst.getInstructionAt(start);
        for (int n = 0; n < count && i != null; n++) {
            out.printf("  %s  %-28s %s%n", i.getAddress(), i.toString(),
                       i.getMnemonicString());
            i = i.getNext();
        }
    }

    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/frame_delta.txt", "UTF-8");

        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        String[] targets = { "00ac33b0", "00ac3040" };
        for (String t : targets) {
            Address a = currentProgram.getAddressFactory().getAddress(t);
            Function f = getFunctionContaining(a);
            out.println("################ FUNCTION @ " + t + " ################");
            if (f == null) { out.println("  no function here"); continue; }
            out.println("name=" + f.getName() + "  entry=" + f.getEntryPoint()
                        + "  size=" + f.getBody().getNumAddresses());
            out.println("---- decompiled ----");
            out.println(decompile(f));
            out.println("---- first 90 instructions ----");
            dumpAsm(out, f.getEntryPoint(), 90);
            out.println();
        }

        // Where do 300000 and 5005 appear as scalars?
        out.println("################ CONSTANT SCAN ################");
        long[] consts = { 300000L, 5005L, 150000L, 10010L };
        Listing lst = currentProgram.getListing();
        InstructionIterator it = lst.getInstructions(true);
        int found = 0;
        while (it.hasNext() && found < 400) {
            Instruction ins = it.next();
            for (int op = 0; op < ins.getNumOperands(); op++) {
                Object[] objs = ins.getOpObjects(op);
                for (Object o : objs) {
                    if (!(o instanceof ghidra.program.model.scalar.Scalar)) continue;
                    long v = ((ghidra.program.model.scalar.Scalar) o).getUnsignedValue();
                    for (long c : consts) {
                        if (v != c) continue;
                        Function ff = getFunctionContaining(ins.getAddress());
                        out.printf("  %-10d at %s  in %-22s : %s%n", c, ins.getAddress(),
                                   ff == null ? "(none)" : ff.getName(), ins.toString());
                        found++;
                    }
                }
            }
        }
        out.println("  (" + found + " scalar hits)");

        // Who calls the delta function?
        out.println("################ CALLERS OF 00ac33b0 ################");
        Address da = currentProgram.getAddressFactory().getAddress("00ac33b0");
        Function df = getFunctionContaining(da);
        if (df != null) {
            for (Reference r : getReferencesTo(df.getEntryPoint())) {
                Function c = getFunctionContaining(r.getFromAddress());
                out.println("  from " + r.getFromAddress() + "  in "
                            + (c == null ? "(none)" : c.getName() + " @ " + c.getEntryPoint())
                            + "  [" + r.getReferenceType() + "]");
            }
        }

        out.close();
        println("wrote frame_delta.txt");
    }
}
