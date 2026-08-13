import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;

// FUN_00b00c00 calls the texture helper FUN_00b00b90 nine-plus times in
// sequence - it is the multi-sample stage's buffer-set allocator, the
// screen-space analogue of the shadow-map allocator FUN_00b010c0. The
// half-res dimensions must be computed here (or arrive as parameters, in
// which case its caller is next). Full decompile + disassembly, looking for
// where 1920/1080 come from at 3840x2160 output.
public class BufferSetAlloc extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/bufferset_alloc.txt","UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();

        Address a = currentProgram.getAddressFactory().getAddress("00b00c00");
        Function f = getFunctionContaining(a);
        if (f == null) { out.println("no function"); out.close(); return; }
        out.println("################ " + f.getName() + " @ " + f.getEntryPoint()
                    + " size=" + f.getBody().getNumAddresses() + " ################");
        DecompileResults r = dec.decompileFunction(f, 240, new ConsoleTaskMonitor());
        String c = (r != null && r.getDecompiledFunction() != null)
                   ? r.getDecompiledFunction().getC() : "(failed)";
        out.println(c);

        out.println("---- full disassembly ----");
        Instruction i = lst.getInstructionAt(f.getEntryPoint());
        long end = f.getBody().getMaxAddress().getOffset();
        int n = 0;
        while (i != null && i.getAddress().getOffset() <= end && n++ < 400) {
            out.printf("  %s  %s%n", i.getAddress(), i.toString());
            i = i.getNext();
        }

        out.println();
        out.println("---- callers ----");
        for (Reference rf : getReferencesTo(f.getEntryPoint())) {
            Function c2 = getFunctionContaining(rf.getFromAddress());
            out.println("  from " + rf.getFromAddress()
                        + (c2 == null ? "" : "  in " + c2.getName() + " @ " + c2.getEntryPoint()));
        }
        out.close();
        println("wrote bufferset_alloc.txt");
    }
}
