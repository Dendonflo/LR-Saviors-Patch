import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;

// The loader measurement run (2026-08-13, Luxerion+Yuusnaan walk) promoted
// the block decrypt to primary fix target: besides being 38% of the
// zone-entry class burst, TWO main-thread calls through FUN_009fcfd0 outside
// any class load totalled ~12.6ms and landed in one frame - a measured 29ms
// gameplay frame, twice in one session.
//
// The fix is a bit-exact fast path: the 8 substitution rounds per byte
// compose, for a fixed key context, into one 256-entry permutation P, so
// decode(byte) = P[raw ^ prev_raw] - one lookup instead of eight. Writing
// that replacement needs the exact calling convention of FUN_009fd110 and of
// its caller FUN_009fcfd0 (stack vs register args, who cleans up), which the
// decompiler's C does not state reliably. So: full disassembly of both.
public class LoaderDecryptAbi extends GhidraScript {
    private void dump(PrintWriter out, String vaStr, String why) {
        Address a = currentProgram.getAddressFactory().getAddress(vaStr);
        Function f = getFunctionContaining(a);
        out.println("################ " + vaStr + "  " +
                    (f != null ? f.getName() + " (" + f.getBody().getNumAddresses() +
                     " bytes)" : "?") + "  - " + why + " ################");
        if (f != null) {
            out.println("   decompiler prototype: " + f.getSignature().getPrototypeString());
            out.println("   calling convention:   " + f.getCallingConventionName());
        }
        Instruction ins = currentProgram.getListing().getInstructionAt(a);
        long end = (f != null) ? f.getBody().getMaxAddress().getOffset() : a.getOffset() + 0x90;
        while (ins != null && ins.getAddress().getOffset() <= end) {
            StringBuilder hex = new StringBuilder();
            try { for (byte b : ins.getBytes()) hex.append(String.format("%02X ", b)); }
            catch (Exception e) { hex.append("??"); }
            out.printf("   %s  %-28s %s%n", ins.getAddress(), hex.toString(), ins.toString());
            ins = ins.getNext();
        }
        out.println();
    }
    @Override public void run() throws Exception {
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/loader_decrypt_abi.txt", "UTF-8");
        dump(out, "009fd110", "the 8-byte block decrypt leaf (replacement target)");
        dump(out, "009fcfd0", "its caller - loops over blocks, reveals how args are passed");
        out.close();
        println("wrote loader_decrypt_abi.txt");
    }
}
