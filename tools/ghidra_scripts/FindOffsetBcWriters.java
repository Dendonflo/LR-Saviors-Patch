import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.Varnode;

import java.io.PrintWriter;
import java.util.HashMap;
import java.util.Map;

// Both per-stage job callbacks found so far (FRAG_00acc120, FRAG_00acba30)
// short-circuit to a no-op if [owner_actor + 0xbc] is null. That field is
// zeroed at actor construction (FUN_00acc310) and never written again by
// anything seen so far. If something else sets it non-null (or clears it)
// at runtime, THAT is the actual per-frame activation switch. Exhaustive
// raw-pcode scan for 4-byte STOREs to [reg + 0xbc], same technique used
// successfully for +0x80 and +0x86.
public class FindOffsetBcWriters extends GhidraScript {
    static final long TARGET_OFFSET = 0xbc;

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/offset_bc_writers.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        AddressSetView execSet = currentProgram.getMemory().getExecuteSet();
        InstructionIterator it = currentProgram.getListing().getInstructions(execSet, true);

        int found = 0;
        int scanned = 0;
        while (it.hasNext()) {
            Instruction insn = it.next();
            scanned++;
            PcodeOp[] ops = insn.getPcode();
            if (ops == null || ops.length == 0) continue;

            Map<String, PcodeOp> producer = new HashMap<>();
            for (PcodeOp op : ops) {
                Varnode outv = op.getOutput();
                if (outv != null) producer.put(varnodeKey(outv), op);
            }

            for (PcodeOp op : ops) {
                if (op.getOpcode() != PcodeOp.STORE) continue;
                Varnode[] inputs = op.getInputs();
                if (inputs.length < 3) continue;
                Varnode addrVn = inputs[1];
                Varnode valueVn = inputs[2];
                if (valueVn.getSize() != 4) continue; // only 32-bit (pointer) stores

                PcodeOp addrProducer = producer.get(varnodeKey(addrVn));
                if (addrProducer == null) continue;
                if (addrProducer.getOpcode() != PcodeOp.INT_ADD) continue;
                Varnode[] addInputs = addrProducer.getInputs();
                boolean matches = false;
                Varnode baseReg = null;
                for (Varnode v : addInputs) {
                    if (v.isConstant() && v.getOffset() == TARGET_OFFSET) matches = true;
                    else baseReg = v;
                }
                if (!matches) continue;

                Function func = getFunctionContaining(insn.getAddress());
                String funcDesc = func != null ? func.getName() + " @ " + func.getEntryPoint() : "NO FUNCTION";
                out.printf("%s : %s   (insn: %s)   base=%s%n",
                        insn.getAddress(), funcDesc, insn.toString(),
                        baseReg != null ? baseReg.toString() : "?");
                found++;
            }
        }

        out.printf("%n--- scanned %d instructions, found %d matching dword-stores to +0xbc ---%n", scanned, found);
        out.close();
        println("DONE: scanned=" + scanned + " found=" + found);
    }

    private static String varnodeKey(Varnode v) {
        return v.getSpace() + ":" + v.getOffset() + ":" + v.getSize();
    }
}
