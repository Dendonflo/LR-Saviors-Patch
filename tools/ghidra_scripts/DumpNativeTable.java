import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import java.io.PrintWriter;

// The native binding table, from FUN_009ddfe0 (the script class loader):
//   ppuVar5 = &PTR_DAT_023193f8;                       // class name ptr
//   pcVar4  = *(char **)(&PTR_DAT_023193fc + iVar6);   // method name ptr
//   *(param_2+6) = (&PTR_FUN_02319400)[iVar7*3];       // native fn ptr
// Stride is 3 pointers (12 bytes). Names are XOR 0xAB - which is why a plain
// string search for them finds nothing.
public class DumpNativeTable extends GhidraScript {
    Memory mem;
    String xorStr(long ptr) {
        if (ptr == 0) return "";
        StringBuilder sb = new StringBuilder();
        try {
            Address a = currentProgram.getAddressFactory().getAddress(Long.toHexString(ptr));
            for (int i = 0; i < 128; i++) {
                int raw = mem.getByte(a.add(i)) & 0xFF;
                if (raw == 0) break;          // terminator is a RAW 0, not XOR'd
                sb.append((char) (raw ^ 0xAB));
            }
        } catch (Exception e) { return "<bad:" + Long.toHexString(ptr) + ">"; }
        return sb.toString();
    }
    @Override public void run() throws Exception {
        mem = currentProgram.getMemory();
        PrintWriter out = new PrintWriter(
          "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/native_table.txt","UTF-8");
        long base = 0x023193f8L;
        int n = 0;
        for (int i = 0; i < 4000; i++) {
            Address ea = currentProgram.getAddressFactory().getAddress(Long.toHexString(base + (long)i * 12));
            long clsPtr, mthPtr, fnPtr;
            try {
                clsPtr = mem.getInt(ea) & 0xFFFFFFFFL;
                mthPtr = mem.getInt(ea.add(4)) & 0xFFFFFFFFL;
                fnPtr  = mem.getInt(ea.add(8)) & 0xFFFFFFFFL;
            } catch (Exception e) { break; }
            if (clsPtr == 0) break;
            String cls = xorStr(clsPtr), mth = xorStr(mthPtr);
            if (cls.isEmpty()) break;
            Function f = null;
            try { f = getFunctionContaining(
                currentProgram.getAddressFactory().getAddress(Long.toHexString(fnPtr))); } catch (Exception e) {}
            out.println(String.format("%-16s %-40s -> %08X %s", cls, mth, fnPtr,
                        f != null ? f.getName() : ""));
            n++;
        }
        out.println("\ntotal natives: " + n);
        out.close(); println("DONE " + n);
    }
}
