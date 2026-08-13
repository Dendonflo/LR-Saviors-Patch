// Same resolution+diff approach as ResolveBaselineAndDiff.java, but for the
// new tightly-scoped capture (single archway crossing, ~2.5s event window vs
// a 3s calm baseline from within the SAME trace/session).

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

public class ResolveTightDiff extends GhidraScript {

    static final long ASLR_OFFSET = 0x00210000L;

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

    @Override
    public void run() throws Exception {
        String eventPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/hot_addresses_tight_event_raw.txt";
        String baselinePath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/hot_addresses_tight_baseline_raw.txt";
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/diff_tight_event_vs_baseline.txt";

        Map<String, Integer> eventHits = resolveFile(eventPath);
        Map<String, Integer> baselineHits = resolveFile(baselinePath);

        int eventTotal = eventHits.values().stream().mapToInt(Integer::intValue).sum();
        int baselineTotal = baselineHits.values().stream().mapToInt(Integer::intValue).sum();

        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        out.printf("Event window total samples: %d%n", eventTotal);
        out.printf("Baseline window total samples: %d%n%n", baselineTotal);

        List<String> allKeys = new ArrayList<>();
        allKeys.addAll(eventHits.keySet());
        for (String k : baselineHits.keySet()) if (!eventHits.containsKey(k)) allKeys.add(k);

        allKeys.sort((a, b) -> {
            double ra = 1000.0 * eventHits.getOrDefault(a, 0) / eventTotal - 1000.0 * baselineHits.getOrDefault(a, 0) / baselineTotal;
            double rb = 1000.0 * eventHits.getOrDefault(b, 0) / eventTotal - 1000.0 * baselineHits.getOrDefault(b, 0) / baselineTotal;
            return Double.compare(rb, ra);
        });

        out.println("=== Diff: event-window vs baseline (sorted by normalized delta, descending) ===");
        out.printf("%-45s %8s %8s %10s %10s %10s%n", "Function", "EvHits", "BaseHits", "Ev/1k", "Base/1k", "Delta");
        for (String k : allKeys) {
            int eh = eventHits.getOrDefault(k, 0);
            int bh = baselineHits.getOrDefault(k, 0);
            if (eh < 2 && bh < 2) continue;
            double eNorm = 1000.0 * eh / eventTotal;
            double bNorm = 1000.0 * bh / baselineTotal;
            out.printf("%-45s %8d %8d %10.2f %10.2f %10.2f%n", k, eh, bh, eNorm, bNorm, eNorm - bNorm);
        }

        out.close();
        println("DONE. Written to " + outPath);
    }
}
