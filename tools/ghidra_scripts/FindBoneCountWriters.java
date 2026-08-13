import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.Varnode;

import java.io.PrintWriter;
import java.util.HashMap;
import java.util.Map;

// Looks for MOV-type stores of a 16-bit (word) value to [reg + 0x80], i.e. the
// exact shape of a write to the "bone/component count" field read by
// FUN_00aacf10 (`*(short *)(param_1 + 0x80)`). Scans raw (low-level) pcode
// per-instruction across the whole executable memory, since the field has no
// recovered struct/datatype to search via normal Ghidra data-type xrefs.
public class FindBoneCountWriters extends GhidraScript {
    static final long TARGET_OFFSET = 0x80;

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/bone_count_writers.txt";
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

            // Map each op's output varnode (space+offset+size key) to the op that produced it,
            // scoped to this single instruction's pcode sequence.
            Map<String, PcodeOp> producer = new HashMap<>();
            for (PcodeOp op : ops) {
                Varnode outv = op.getOutput();
                if (outv != null) {
                    producer.put(varnodeKey(outv), op);
                }
            }

            for (PcodeOp op : ops) {
                if (op.getOpcode() != PcodeOp.STORE) continue;
                Varnode[] inputs = op.getInputs();
                if (inputs.length < 3) continue;
                Varnode addrVn = inputs[1];
                Varnode valueVn = inputs[2];
                if (valueVn.getSize() != 2) continue; // only 16-bit (short) stores

                // Trace back: is addrVn produced by INT_ADD(reg, 0x80) in this same instruction?
                PcodeOp addrProducer = producer.get(varnodeKey(addrVn));
                if (addrProducer == null) continue;
                if (addrProducer.getOpcode() != PcodeOp.INT_ADD) continue;
                Varnode[] addInputs = addrProducer.getInputs();
                boolean matches = false;
                Varnode baseReg = null;
                for (int i = 0; i < addInputs.length; i++) {
                    Varnode v = addInputs[i];
                    if (v.isConstant() && v.getOffset() == TARGET_OFFSET) {
                        matches = true;
                    } else {
                        baseReg = v;
                    }
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

        out.printf("%n--- scanned %d instructions, found %d matching word-stores to +0x80 ---%n", scanned, found);
        out.close();
        println("DONE: scanned=" + scanned + " found=" + found);
    }

    private static String varnodeKey(Varnode v) {
        return v.getSpace() + ":" + v.getOffset() + ":" + v.getSize();
    }
}
