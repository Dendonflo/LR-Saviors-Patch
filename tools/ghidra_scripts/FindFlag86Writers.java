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

// FUN_00acbe70 gates the expensive per-frame skeleton-transform call on two
// bits of the byte at [object + 0x86] (bit 0x8 must be set, bit 0x20 must be
// clear) plus a pointer chase through +0xb0/+0xa0. Looking for whoever WRITES
// (not just reads) that flag byte -- same exhaustive raw-pcode scan technique
// used successfully for the bone-count field (+0x80). Byte-sized (1-byte)
// STOREs to [reg + 0x86] only.
public class FindFlag86Writers extends GhidraScript {
    static final long TARGET_OFFSET = 0x86;

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/flag86_writers.txt";
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
                if (valueVn.getSize() != 1) continue; // only 8-bit (byte) stores

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

        out.printf("%n--- scanned %d instructions, found %d matching byte-stores to +0x86 ---%n", scanned, found);
        out.close();
        println("DONE: scanned=" + scanned + " found=" + found);
    }

    private static String varnodeKey(Varnode v) {
        return v.getSpace() + ":" + v.getOffset() + ":" + v.getSize();
    }
}
