// Same resolve+diff methodology as before, but for the combined
// PresentMon+xperf(stackwalk) capture, using ground-truth-confirmed event
// windows (derived from actual measured bad frames, not indirect
// utilization guessing). ASLR offset for THIS session's process
// (module base 0x150000) is +0x2B0000 (runtime -> ghidra), i.e.
// ghidra_addr = runtime_addr + 0x2B0000, since 0x150000 < preferred base
// 0x00400000 this time -- different from earlier sessions, confirmed via
// live module base read (Toolhelp32) at analysis time, process start time
// cross-checked against capture file timestamps.

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

public class ResolveCombinedDiff extends GhidraScript {

    static final long ASLR_ADJUST = 0x002B0000L; // ghidra = runtime + this

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
            long ghidraAddr = runtimeAddr + ASLR_ADJUST;
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
        String base = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/";
        Map<String, Integer> eventHits = resolveFile(base + "hot_addresses_combined_event_raw.txt");
        Map<String, Integer> baselineHits = resolveFile(base + "hot_addresses_combined_baseline_raw.txt");

        int eventTotal = eventHits.values().stream().mapToInt(Integer::intValue).sum();
        int baselineTotal = baselineHits.values().stream().mapToInt(Integer::intValue).sum();

        PrintWriter out = new PrintWriter(base + "diff_combined_event_vs_baseline.txt", "UTF-8");
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
        for (String k : allKeys) {
            int eh = eventHits.getOrDefault(k, 0);
            int bh = baselineHits.getOrDefault(k, 0);
            if (eh < 3 && bh < 3) continue;
            double eNorm = 1000.0 * eh / eventTotal;
            double bNorm = 1000.0 * bh / baselineTotal;
            out.printf("%-40s %8d %8d %10.2f %10.2f %10.2f%n", k, eh, bh, eNorm, bNorm, eNorm - bNorm);
        }

        out.close();
        println("DONE");
    }
}
