import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.util.*;

// FieldTalkManager::update is confirmed per-frame. It reads its time base from
// [manager+0x10] and stamps deadlines as "base + 125". Find who writes
// [manager+0x10] - if it is incremented by 1 per frame it is a frame counter
// and every deadline expressed in it halves at 60fps.
//
// FUN_005dbc10 is called immediately before update in the field state machine,
// which is the shape of a "advance the manager clock" call.
public class TalkClock extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/talk_clock.txt","UTF-8");
        DecompInterface d = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.setMaxPayloadMBytes(128);
        d.setOptions(opts);
        d.openProgram(currentProgram);

        String[] targets = { "005dbc10", "005d9880", "005b4fb0", "005b41c0" };
        for (String t : targets) {
            Address a = currentProgram.getAddressFactory().getAddress(t);
            Function f = getFunctionContaining(a);
            if (f == null) { out.println("no function at " + t); continue; }
            out.println("================ " + f.getName() + " @ " + f.getEntryPoint()
                        + " size=" + f.getBody().getNumAddresses() + " ================");
            DecompileResults dr = d.decompileFunction(f, 600, new ConsoleTaskMonitor());
            out.println(dr != null && dr.decompileCompleted()
                        ? dr.getDecompiledFunction().getC()
                        : "FAILED");
            out.flush();
        }

        // Raw scan: every instruction in the whole binary that stores to
        // [reg+0x10] with an INC-like shape, restricted to the field module
        // address range so the output stays readable.
        out.println("\n######## INC/ADD 1 into [reg+0x10] in the field code range ########");
        Address lo = currentProgram.getAddressFactory().getAddress("005a0000");
        Address hi = currentProgram.getAddressFactory().getAddress("00620000");
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext()) {
            Instruction i = it.next();
            if (i.getAddress().compareTo(lo) < 0 || i.getAddress().compareTo(hi) > 0) continue;
            String s = i.toString();
            if (!s.contains("0x10]")) continue;
            String m = i.getMnemonicString();
            if (!m.equals("INC") && !m.equals("ADD") && !m.equals("MOV")) continue;
            Function f = getFunctionContaining(i.getAddress());
            out.println("  " + i.getAddress() + "  " + s
                        + (f == null ? "" : "   in " + f.getName()));
        }
        d.dispose(); out.close(); println("DONE");
    }
}
