// Locates the exact instruction (address + bytes) that assigns the literal
// 4 to DAT_02350664 inside WinMain (FUN_00d12da0), and computes the
// corresponding raw FILE offset (not just the Ghidra virtual address) using
// the PE section headers, since patching the .exe on disk requires the file
// offset, which differs from the virtual address due to section
// alignment/padding.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;

import java.io.PrintWriter;

public class FindPatchLocation extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/patch_location.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        Address funcAddr = currentProgram.getAddressFactory().getAddress("00d12da0");
        Function func = getFunctionContaining(funcAddr);
        Address target = currentProgram.getAddressFactory().getAddress("02350664");

        out.println("=== Scanning instructions in FUN_00d12da0 for references to DAT_02350664 ===");
        InstructionIterator ii = currentProgram.getListing().getInstructions(func.getBody(), true);
        for (Instruction instr : ii) {
            boolean refsTarget = false;
            for (Address refAddr : instr.getFlows()) {
                if (refAddr.equals(target)) refsTarget = true;
            }
            // Check operand references too (data refs, not just flows)
            ghidra.program.model.symbol.Reference[] refs = instr.getReferencesFrom();
            for (ghidra.program.model.symbol.Reference r : refs) {
                if (r.getToAddress().equals(target)) refsTarget = true;
            }
            if (refsTarget) {
                byte[] b = instr.getBytes();
                StringBuilder hex = new StringBuilder();
                for (byte bb : b) hex.append(String.format("%02x ", bb));
                out.printf("FOUND @ %s : %s   bytes=%s length=%d%n",
                    instr.getAddress(), instr.toString(),
                    hex.toString().trim(), instr.getLength());
            }
        }

        out.println();
        out.println("=== PE section headers (for RVA -> file offset conversion) ===");
        MemoryBlock block = currentProgram.getMemory().getBlock(target);
        out.printf("Target 0x%s is in memory block: %s (start=%s, end=%s)%n",
            target, block != null ? block.getName() : "?",
            block != null ? block.getStart() : "?", block != null ? block.getEnd() : "?");

        decomp_note(out);

        out.close();
        println("DONE");
    }

    void decomp_note(PrintWriter out) {
        out.println();
        out.println("(If PE section parsing isn't directly available via this API path,");
        out.println("the block start/end above plus the known preferred image base 0x00400000");
        out.println("is enough to compute the file offset manually once we check the PE");
        out.println("section table separately.)");
    }
}
