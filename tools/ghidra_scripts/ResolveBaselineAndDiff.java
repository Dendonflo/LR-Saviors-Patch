// Resolves the baseline (quiet-period) hot addresses the same way as
// ResolveHotAddresses.java did for the stutter window, then loads the
// already-resolved stutter-window function hit counts and prints a direct
// side-by-side diff so we can see which functions are "always hot" (present
// in both, similar magnitude) versus genuinely stutter-specific (high in the
// stutter window, near-zero in the baseline).

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

public class ResolveBaselineAndDiff extends GhidraScript {

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
        String stutterPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/hot_addresses_raw.txt";
        String baselinePath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/hot_addresses_baseline_raw.txt";
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/diff_stutter_vs_baseline.txt";

        Map<String, Integer> stutterHits = resolveFile(stutterPath);
        Map<String, Integer> baselineHits = resolveFile(baselinePath);

        int stutterTotal = stutterHits.values().stream().mapToInt(Integer::intValue).sum();
        int baselineTotal = baselineHits.values().stream().mapToInt(Integer::intValue).sum();

        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        out.printf("Stutter window total samples: %d%n", stutterTotal);
        out.printf("Baseline window total samples: %d%n%n", baselineTotal);

        // Union of all function keys, sorted by stutter hit count descending
        List<String> allKeys = new ArrayList<>();
        allKeys.addAll(stutterHits.keySet());
        for (String k : baselineHits.keySet()) if (!stutterHits.containsKey(k)) allKeys.add(k);

        allKeys.sort(Collections.reverseOrder(Comparator.comparing(k -> stutterHits.getOrDefault(k, 0))));

        out.println("=== Diff: stutter-window hits vs baseline hits (normalized per 1000 samples) ===");
        out.printf("%-45s %10s %10s %12s %12s%n", "Function", "StutterHits", "BaseHits", "Stutter/1k", "Base/1k");
        for (String k : allKeys) {
            int sh = stutterHits.getOrDefault(k, 0);
            int bh = baselineHits.getOrDefault(k, 0);
            if (sh < 2 && bh < 2) continue;
            double sNorm = 1000.0 * sh / stutterTotal;
            double bNorm = 1000.0 * bh / baselineTotal;
            out.printf("%-45s %10d %10d %12.2f %12.2f%n", k, sh, bh, sNorm, bNorm);
        }

        out.close();
        println("DONE. Written to " + outPath);
    }
}
