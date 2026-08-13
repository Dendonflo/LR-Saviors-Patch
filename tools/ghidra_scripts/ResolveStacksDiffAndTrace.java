// Full pipeline for the properly stack-walked capture:
//   1) Resolve+diff event vs baseline leaf-address hit counts (as before).
//   2) For the top-N functions by positive delta (event-heavier), pull their
//      actual call stacks from the event-window dump (now correctly
//      correlated via -stackwalk Profile) and print aggregated, resolved
//      call chains -- this tells us what ACTUALLY calls into them, instead
//      of guessing from decompiled code alone.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Comparator;

public class ResolveStacksDiffAndTrace extends GhidraScript {

    static final long ASLR_OFFSET = 0x00210000L;
    static final int TOP_N_FOR_STACKS = 10;

    Map<String, Integer> resolveFile(String path) throws Exception {
        BufferedReader in = new BufferedReader(new FileReader(path));
        Map<String, Integer> funcHits = new HashMap<>();
        String line;
        while ((line = in.readLine()) != null) {
            line = line.trim();
            if (line.isEmpty()) continue;
            String[] parts = line.split("\\s+");
            if (parts.length != 2) continue;
            int count = Integer.parseInt(parts[0]);
            long runtimeAddr = Long.parseLong(parts[1], 16);
            long ghidraAddr = runtimeAddr - ASLR_OFFSET;
            Address addr;
            try {
                addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(ghidraAddr);
            } catch (Exception e) {
                continue;
            }
            Function func = getFunctionContaining(addr);
            String key = func != null ? func.getName() + " @ " + func.getEntryPoint() : "NO_FUNCTION @ " + addr;
            funcHits.merge(key, count, Integer::sum);
        }
        in.close();
        return funcHits;
    }

    String resolveRuntimeAddr(long runtimeAddr) {
        long ghidraAddr = runtimeAddr - ASLR_OFFSET;
        try {
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(ghidraAddr);
            Function f = getFunctionContaining(addr);
            return f != null ? f.getName() + "@" + f.getEntryPoint() : ("NO_FUNC@0x" + Long.toHexString(ghidraAddr));
        } catch (Exception e) {
            return "BAD_ADDR";
        }
    }

    @Override
    public void run() throws Exception {
        String base = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/";
        Map<String, Integer> eventHits = resolveFile(base + "hot_addresses_stacks_event_raw.txt");
        Map<String, Integer> baselineHits = resolveFile(base + "hot_addresses_stacks_baseline_raw.txt");

        int eventTotal = eventHits.values().stream().mapToInt(Integer::intValue).sum();
        int baselineTotal = baselineHits.values().stream().mapToInt(Integer::intValue).sum();

        PrintWriter out = new PrintWriter(base + "diff_stacks_event_vs_baseline.txt", "UTF-8");
        out.printf("Event window total: %d, Baseline window total: %d%n%n", eventTotal, baselineTotal);

        List<String> allKeys = new ArrayList<>();
        allKeys.addAll(eventHits.keySet());
        for (String k : baselineHits.keySet()) if (!eventHits.containsKey(k)) allKeys.add(k);

        allKeys.sort((a, b) -> {
            double ra = 1000.0 * eventHits.getOrDefault(a, 0) / eventTotal - 1000.0 * baselineHits.getOrDefault(a, 0) / baselineTotal;
            double rb = 1000.0 * eventHits.getOrDefault(b, 0) / eventTotal - 1000.0 * baselineHits.getOrDefault(b, 0) / baselineTotal;
            return Double.compare(rb, ra);
        });

        out.printf("%-40s %8s %8s %10s %10s %10s%n", "Function", "EvHits", "BaseHits", "Ev/1k", "Base/1k", "Delta");
        List<String> topFunctionKeys = new ArrayList<>();
        for (String k : allKeys) {
            int eh = eventHits.getOrDefault(k, 0);
            int bh = baselineHits.getOrDefault(k, 0);
            if (eh < 2 && bh < 2) continue;
            double eNorm = 1000.0 * eh / eventTotal;
            double bNorm = 1000.0 * bh / baselineTotal;
            out.printf("%-40s %8d %8d %10.2f %10.2f %10.2f%n", k, eh, bh, eNorm, bNorm, eNorm - bNorm);
            if (topFunctionKeys.size() < TOP_N_FOR_STACKS && eh >= 3) topFunctionKeys.add(k);
        }

        out.println();
        out.println("=== Call stacks for top functions (from event window, resolved) ===");

        // Build entry-address -> label map for the functions we'll trace
        Map<String, String> entryToKey = new LinkedHashMap<>();
        for (String k : topFunctionKeys) {
            String entryStr = k.substring(k.lastIndexOf('@') + 1).trim();
            entryToKey.put(entryStr, k);
        }

        // Pass 1: find (timestamp|threadid) keys in the event dump whose leaf resolves to one of our targets
        String dumpPath = base + "dump_stacks_event.csv";
        Map<String, String> keyToLabel = new LinkedHashMap<>();
        BufferedReader in1 = new BufferedReader(new FileReader(dumpPath));
        String line;
        while ((line = in1.readLine()) != null) {
            if (!line.contains("SampledProfile,")) continue;
            if (!line.contains("LRFF13.exe (")) continue;
            String[] parts = line.split(",");
            if (parts.length < 8) continue;
            String imageFunc = parts[7].trim();
            if (!imageFunc.startsWith("LRFF13.exe!0x")) continue;
            long runtimeAddr;
            try {
                runtimeAddr = Long.parseLong(imageFunc.substring("LRFF13.exe!0x".length()), 16);
            } catch (Exception e) { continue; }
            long ghidraAddr = runtimeAddr - ASLR_OFFSET;
            Address addr;
            Function f;
            try {
                addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(ghidraAddr);
                f = getFunctionContaining(addr);
            } catch (Exception e) { continue; }
            if (f == null) continue;
            String entryStr = f.getEntryPoint().toString();
            if (entryToKey.containsKey(entryStr)) {
                String timestamp = parts[1].trim();
                String threadId = parts[3].trim();
                keyToLabel.put(timestamp + "|" + threadId, entryToKey.get(entryStr));
            }
        }
        in1.close();

        out.printf("(%d target leaf-samples matched for stack tracing)%n%n", keyToLabel.size());

        // Pass 2: collect stack frames for those keys
        Map<String, List<String>> stacksByKey = new LinkedHashMap<>();
        BufferedReader in2 = new BufferedReader(new FileReader(dumpPath));
        while ((line = in2.readLine()) != null) {
            String trimmed = line.trim();
            if (!trimmed.startsWith("Stack,") && !trimmed.startsWith("Stack ")) continue;
            // Handle both comma and whitespace-only formatting robustly
            String[] parts = line.split(",");
            if (parts.length < 5) continue;
            String timestamp = parts[1].trim();
            String threadId = parts[2].trim();
            String key = timestamp + "|" + threadId;
            if (!keyToLabel.containsKey(key)) continue;
            String imageFunc = parts[parts.length - 1].trim();
            stacksByKey.computeIfAbsent(key, k -> new ArrayList<>()).add(imageFunc);
        }
        in2.close();

        Map<String, Map<String, Integer>> chainCountsByLabel = new LinkedHashMap<>();
        for (Map.Entry<String, String> e : keyToLabel.entrySet()) {
            List<String> frames = stacksByKey.get(e.getKey());
            if (frames == null) continue;
            List<String> resolvedChain = new ArrayList<>();
            for (String imageFunc : frames) {
                if (imageFunc.startsWith("LRFF13.exe!0x")) {
                    long runtimeAddr;
                    try {
                        runtimeAddr = Long.parseLong(imageFunc.substring("LRFF13.exe!0x".length()), 16);
                    } catch (Exception ex) { continue; }
                    resolvedChain.add(resolveRuntimeAddr(runtimeAddr));
                } else {
                    resolvedChain.add(imageFunc);
                }
            }
            String chainStr = String.join(" <- ", resolvedChain);
            chainCountsByLabel.computeIfAbsent(e.getValue(), k -> new HashMap<>()).merge(chainStr, 1, Integer::sum);
        }

        for (String labelKey : topFunctionKeys) {
            out.printf("--- %s ---%n", labelKey);
            Map<String, Integer> chains = chainCountsByLabel.get(labelKey);
            if (chains == null || chains.isEmpty()) {
                out.println("  (no stacks captured for this function's samples)");
                out.println();
                continue;
            }
            List<Map.Entry<String, Integer>> sorted = new ArrayList<>(chains.entrySet());
            sorted.sort((a, b) -> b.getValue() - a.getValue());
            for (Map.Entry<String, Integer> c : sorted) {
                out.printf("  [%d] %s%n", c.getValue(), c.getKey());
            }
            out.println();
        }

        out.close();
        println("DONE");
    }
}
