// Broad net search for asset-loading/streaming related strings and named
// symbols in LRFF13.exe, so we can locate the resource/streaming subsystem
// without knowing class names up front. Mirrors the approach used for the
// BL4 music-mod project's FindAkCallers.java.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;
import ghidra.program.model.mem.MemoryBlock;

import java.io.PrintWriter;

public class FindStreamingStrings extends GhidraScript {

    static final String[] KEYWORDS = {
        "resource", "stream", "asyncload", "async_load", "loadasync",
        "asynctask", "loadthread", "loadjob", "prefetch", "preload",
        "unload", "archive", "wdb", "assetmanager", "resourcemanager",
        "loadmap", "loadarea", "loadzone", "loadlevel", "loadfile",
        "readfile", "decompress", "zlib", "loadtexture", "loadmodel",
        "loadmesh", "chunk", "streaming", "background load", "bgload",
        "imgb", "wpd", "cri", "criware"
    };

    boolean matches(String s) {
        String low = s.toLowerCase();
        for (String k : KEYWORDS) {
            if (low.contains(k)) return true;
        }
        return false;
    }

    String blockName(Address a) {
        MemoryBlock b = currentProgram.getMemory().getBlock(a);
        return b == null ? "?" : b.getName();
    }

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/tools/ghidra_output/streaming_strings.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        out.println("=== PART 1: named functions/symbols matching streaming/resource keywords ===");
        SymbolTable st = currentProgram.getSymbolTable();
        SymbolIterator symIter = st.getAllSymbols(true);
        int symCount = 0;
        while (symIter.hasNext()) {
            Symbol sym = symIter.next();
            String name = sym.getName();
            if (name == null || !matches(name)) continue;
            symCount++;
            Address addr = sym.getAddress();
            out.printf("SYMBOL: %s @ %s (block=%s, type=%s)%n", name, addr, blockName(addr), sym.getSymbolType());

            Function func = getFunctionAt(addr);
            if (func != null) {
                Reference[] refs = getReferencesTo(addr);
                for (Reference ref : refs) {
                    Address fromAddr = ref.getFromAddress();
                    Function callerFunc = getFunctionContaining(fromAddr);
                    String callerName = callerFunc != null ? callerFunc.getName() + " @ " + callerFunc.getEntryPoint() : "(no function, block=" + blockName(fromAddr) + ")";
                    out.printf("    referenced from %s in %s%n", fromAddr, callerName);
                }
            }
        }
        out.printf("TOTAL MATCHING SYMBOLS: %d%n%n", symCount);

        out.println("=== PART 2: defined strings matching streaming/resource keywords ===");
        DataIterator dataIter = currentProgram.getListing().getDefinedData(true);
        int strCount = 0;
        while (dataIter.hasNext()) {
            Data data = dataIter.next();
            if (!data.hasStringValue()) continue;
            Object val;
            try { val = data.getValue(); } catch (Exception e) { continue; }
            if (val == null) continue;
            String sval = val.toString();
            if (sval.length() < 3 || !matches(sval)) continue;

            strCount++;
            Address addr = data.getAddress();
            out.printf("STRING @ %s (block=%s): %s%n", addr, blockName(addr), sval);

            Reference[] refs = getReferencesTo(addr);
            for (Reference ref : refs) {
                Address fromAddr = ref.getFromAddress();
                Function func = getFunctionContaining(fromAddr);
                String fname = func != null ? func.getName() + " @ " + func.getEntryPoint() : "(no function, block=" + blockName(fromAddr) + ")";
                out.printf("    ref from %s in %s%n", fromAddr, fname);
            }
        }
        out.printf("TOTAL MATCHING STRINGS: %d%n", strCount);

        out.close();
        println("DONE. " + symCount + " symbols, " + strCount + " strings written to " + outPath);
    }
}
