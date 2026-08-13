// Reads hot_addresses_raw.txt (lines: "<count> <runtime_hex_addr_no_0x>"), produced
// from a WPR CPU-sampling trace filtered to LRFF13.exe's own leaf samples during a
// reproduced stutter. This session's runtime module base was confirmed (via the
// live memory monitor) to be 0x00610000, vs. Ghidra's load base of 0x00400000, so
// ghidra_addr = runtime_addr - 0x00210000. Resolves each to its containing function
// and aggregates hit counts per function, so we can see which function(s) are
// actually burning CPU time during the stutter.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Collections;
import java.util.Comparator;

public class ResolveHotAddresses extends GhidraScript {

    static final long ASLR_OFFSET = 0x00210000L;

    @Override
    public void run() throws Exception {
        String inPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/hot_addresses_raw.txt";
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/hot_functions_resolved.txt";

        BufferedReader in = new BufferedReader(new FileReader(inPath));
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        Map<String, Integer> funcHits = new HashMap<>();
        Map<String, List<String>> funcAddrDetail = new HashMap<>();
        int unresolved = 0;
        int totalLines = 0;

        String line;
        while ((line = in.readLine()) != null) {
            line = line.trim();
            if (line.isEmpty()) continue;
            String[] parts = line.split("\\s+");
            if (parts.length != 2) continue;
            totalLines++;
            int count = Integer.parseInt(parts[0]);
            long runtimeAddr = Long.parseLong(parts[1], 16);
            long ghidraAddr = runtimeAddr - ASLR_OFFSET;

            Address addr;
            try {
                addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(ghidraAddr);
            } catch (Exception e) {
                unresolved++;
                continue;
            }

            Function func = getFunctionContaining(addr);
            String key;
            if (func != null) {
                key = func.getName() + " @ " + func.getEntryPoint();
            } else {
                key = "NO_FUNCTION @ " + addr;
                unresolved++;
            }

            funcHits.merge(key, count, Integer::sum);
            funcAddrDetail.computeIfAbsent(key, k -> new ArrayList<>())
                .add(String.format("0x%08x (ghidra) / 0x%08x (runtime) : %d hits", ghidraAddr, runtimeAddr, count));
        }
        in.close();

        List<Map.Entry<String, Integer>> sorted = new ArrayList<>(funcHits.entrySet());
        sorted.sort(Collections.reverseOrder(Comparator.comparing(Map.Entry::getValue)));

        out.printf("Total address lines: %d, unresolved: %d%n%n", totalLines, unresolved);
        out.println("=== Functions ranked by total sample hits ===");
        for (Map.Entry<String, Integer> e : sorted) {
            out.printf("%5d hits : %s%n", e.getValue(), e.getKey());
        }

        out.println();
        out.println("=== Detail per function (top 15) ===");
        int shown = 0;
        for (Map.Entry<String, Integer> e : sorted) {
            if (shown++ >= 15) break;
            out.printf("--- %s (%d total hits) ---%n", e.getKey(), e.getValue());
            List<String> details = funcAddrDetail.get(e.getKey());
            details.sort((a, b) -> {
                int ca = Integer.parseInt(a.substring(a.lastIndexOf(':') + 2, a.indexOf(" hits")));
                int cb = Integer.parseInt(b.substring(b.lastIndexOf(':') + 2, b.indexOf(" hits")));
                return cb - ca;
            });
            for (String d : details) {
                out.println("    " + d);
            }
        }

        out.close();
        println("DONE. Written to " + outPath);
    }
}
