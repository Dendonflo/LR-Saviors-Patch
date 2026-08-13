import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

// Runtime bisection put the death in loop 3 of sfShowWindowWithKeyWait:
//     Window.hideWindow(n);                                  <-- REACHED (logged)
//     while (Window.isWindowClosing((String)name)) { delay(); }   <-- dies here
//
// Loop 3 passes a STRING, so it takes the by-NAME arm of isWindowClosing_l:
//     if (selector == 0) FUN_00795df0(id);     // by ID   - verified safe earlier
//     else               FUN_00795f50(name);   // by NAME - NEVER CHECKED
//
// Earlier I verified only the by-ID function and treated it as covering both.
// Decompile AND disassemble the by-name one: if it returns true (or garbage)
// when the window is already gone, the loop never terminates and every
// statement after the message box - the timers and the plant activation - is
// dead. That is exactly what the log shows.
public class ClosingByName extends GhidraScript {
    private void dump(PrintWriter out, String addr, String label) throws Exception {
        Address a = currentProgram.getAddressFactory().getAddress(addr);
        Function f = getFunctionContaining(a);
        if (f == null) { out.println("no function at " + addr); return; }
        out.println("################ " + label + "  " + f.getName()
                    + " @ " + f.getEntryPoint()
                    + " size=" + f.getBody().getNumAddresses() + " ################");
        InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
        while (it.hasNext()) {
            Instruction i = it.next();
            StringBuilder sb = new StringBuilder("  " + i.getAddress() + "  " + i);
            for (Reference r : i.getReferencesFrom()) {
                if (r.getReferenceType().isCall()) {
                    Function t = getFunctionAt(r.getToAddress());
                    if (t != null) sb.append("        ; -> " + t.getName());
                }
            }
            out.println(sb);
        }
        out.println();
    }

    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/closing_by_name.txt","UTF-8");
        dump(out, "00795f50", "isWindowClosing BY NAME (loop 3) - THE SUSPECT");
        dump(out, "00795df0", "isWindowClosing BY ID - verified safe, for contrast");
        out.close();
        println("DONE");
    }
}
