import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// Raw-stack scan at the half-res CreateTexture returned these game-module
// addresses: 00B454DE, 00B4551C, 00B4559A (plus 01970002 / 01E7022D, which are
// stack garbage that happens to fall inside the module range - the known cost
// of scanning rather than walking, since this exe is built with frame-pointer
// omission).
//
// Those three sit above the generic texture plumbing (FUN_00aa3960 /
// FUN_00aa3ce0 / FUN_00aa3010), so the /2 should be visible here. Looking for:
//   - a shift/divide on the presentation width and height
//   - or a field read that supplies the divisor (writeable = the ShadowMapRes
//     pattern, which is the fix shape that has actually worked on this engine)
public class HalfResOwner extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/halfres_owner.txt","UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing lst = currentProgram.getListing();

        String[] addrs = { "00b454de", "00b4551c", "00b4559a" };
        Set<String> done = new LinkedHashSet<>();
        for (String s : addrs) {
            Address a = currentProgram.getAddressFactory().getAddress(s);
            Function f = getFunctionContaining(a);
            if (f == null) { out.println("### " + s + ": no function"); continue; }
            String key = f.getEntryPoint().toString();
            if (!done.add(key)) {
                out.println("### " + s + " is inside " + f.getName() + " (already dumped)");
                continue;
            }
            out.println("################ " + f.getName() + " @ " + f.getEntryPoint()
                        + "  (contains " + s + ") ################");
            out.println("size=" + f.getBody().getNumAddresses());
            DecompileResults r = dec.decompileFunction(f, 180, new ConsoleTaskMonitor());
            String c = (r != null && r.getDecompiledFunction() != null)
                       ? r.getDecompiledFunction().getC() : "(failed)";
            if (c.length() > 14000) c = c.substring(0, 14000) + "\n... [truncated]";
            out.println(c);

            out.println("---- shifts / divides / small immediates ----");
            InstructionIterator it = lst.getInstructions(f.getBody(), true);
            while (it.hasNext()) {
                Instruction i = it.next();
                String t = i.toString();
                if (t.startsWith("SHR") || t.startsWith("SAR") || t.startsWith("IDIV")
                    || t.startsWith("DIV") || t.startsWith("IMUL") || t.startsWith("MUL")
                    || t.contains(",0x2") || t.contains(",0x1"))
                    out.println("    " + i.getAddress() + "  " + t);
            }
            out.println();
        }
        out.close();
        println("wrote halfres_owner.txt");
    }
}
