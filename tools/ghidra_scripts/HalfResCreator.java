import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// Runtime trace: every half-presentation-size render target (1920x1080 at
// 3840x2160 output - the screen-space shadow buffers) is created from ONE
// return address, 00AA3BB1. Find where the /2 comes from.
//
// What matters is the SHAPE of the size calculation:
//   - reads a settings field  -> write the field, engine recomputes downstream
//                                (the ShadowMapRes pattern, which worked)
//   - a literal shift/divide  -> instruction patch, more invasive
//   - passed in by the caller -> follow one level up
//
// ShadowScale failed by growing the surface while its consumers kept the old
// size. Whatever is found here, the same question applies: who ELSE reads
// these dimensions?
public class HalfResCreator extends GhidraScript {
    private DecompInterface dec;

    private void dump(PrintWriter out, String addr, String label, int asmCount) {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        Function f = getFunctionContaining(a);
        out.println("################ " + label + " @ " + addr + " ################");
        if (f == null) { out.println("  no function containing this address"); return; }
        out.println("function " + f.getName() + " entry=" + f.getEntryPoint()
                    + " size=" + f.getBody().getNumAddresses());
        DecompileResults r = dec.decompileFunction(f, 180, new ConsoleTaskMonitor());
        String c = (r != null && r.getDecompiledFunction() != null)
                   ? r.getDecompiledFunction().getC() : "(decompile failed)";
        if (c.length() > 12000) c = c.substring(0, 12000) + "\n... [truncated]";
        out.println(c);

        // Instructions around the call site - the divide/shift should be right
        // before the CreateTexture arguments are pushed.
        out.println("---- instructions around the call site ----");
        Listing lst = currentProgram.getListing();
        Instruction i = lst.getInstructionAt(a);
        for (int n = 0; n < 30 && i != null; n++) i = i.getPrevious();
        for (int n = 0; n < asmCount && i != null; n++) {
            String mark = i.getAddress().equals(a) ? "  <== return addr" : "";
            out.printf("  %s  %-34s%s%n", i.getAddress(), i.toString(), mark);
            i = i.getNext();
        }
        out.println();
        out.println("---- callers ----");
        for (Reference rf : getReferencesTo(f.getEntryPoint())) {
            Function c2 = getFunctionContaining(rf.getFromAddress());
            out.println("  from " + rf.getFromAddress()
                        + (c2 == null ? "" : "  in " + c2.getName() + " @ " + c2.getEntryPoint()));
        }
        out.println();
    }

    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/halfres_creator.txt","UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        dump(out, "00aa3bb1", "half-res RT creator (CreateTexture return address)", 60);
        out.close();
        println("wrote halfres_creator.txt");
    }
}
