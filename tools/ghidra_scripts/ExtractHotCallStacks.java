// Correlates SampledProfile leaf-samples that landed in specific target
// functions (within the tight event window) with their full call stacks,
// by matching (TimeStamp, ThreadID) against the trace's "Stack," event
// lines (dumped via `xperf -a dumper`). This answers "what actually called
// into this hot function during the stutter" instead of just "this function
// showed up as a leaf sample" -- the rigorous next step after leaf-only
// profiling gave an ambiguous/diluted picture.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.LinkedHashMap;

public class ExtractHotCallStacks extends GhidraScript {

    static final long ASLR_OFFSET = 0x00210000L;

    // Ghidra addresses of the functions we want call-stack context for.
    static final String[][] TARGETS = {
        {"00b8edb0", "AABB_ray_signselect"},
        {"0040b1e0", "small_math_util"},
        {"00416140", "quaternion_multiply"},
        {"004164f0", "quat_to_matrix"},
        {"00a015b0", "spinlock_pool_alloc"},
        {"00ac3040", "frame_pacer_wait"}
    };

    String resolveGhidra(long ghidraAddr) {
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
        String dumpPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/dump_tight_event.csv";
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/hot_call_stacks.txt";

        // Map from target Ghidra entry address string -> label
        Map<String, String> targetLabels = new LinkedHashMap<>();
        Map<String, Long> targetEntry = new LinkedHashMap<>();
        for (String[] t : TARGETS) {
            long gAddr = Long.parseLong(t[0], 16);
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(gAddr);
            Function f = getFunctionContaining(addr);
            if (f == null) continue;
            String key = f.getEntryPoint().toString();
            targetLabels.put(key, t[1]);
            targetEntry.put(key, f.getEntryPoint().getOffset());
        }

        // Pass 1: find (TimeStamp, ThreadID) keys for SampledProfile leaf
        // samples in LRFF13.exe that resolve to one of our target functions.
        Map<String, String> keyToLabel = new LinkedHashMap<>(); // "timestamp|threadid" -> label
        BufferedReader in1 = new BufferedReader(new FileReader(dumpPath));
        String line;
        int scanned = 0;
        while ((line = in1.readLine()) != null) {
            if (!line.contains("SampledProfile,")) continue;
            if (!line.contains("LRFF13.exe (")) continue;
            String[] parts = line.split(",");
            if (parts.length < 8) continue;
            String timestamp = parts[1].trim();
            String threadId = parts[3].trim();
            String imageFunc = parts[7].trim();
            if (!imageFunc.startsWith("LRFF13.exe!0x")) continue;
            long runtimeAddr = Long.parseLong(imageFunc.substring("LRFF13.exe!0x".length()), 16);
            long ghidraAddr = runtimeAddr - ASLR_OFFSET;
            Address addr;
            Function f;
            try {
                addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(ghidraAddr);
                f = getFunctionContaining(addr);
            } catch (Exception e) {
                continue;
            }
            if (f == null) continue;
            String entryKey = f.getEntryPoint().toString();
            if (targetLabels.containsKey(entryKey)) {
                keyToLabel.put(timestamp + "|" + threadId, targetLabels.get(entryKey));
                scanned++;
            }
        }
        in1.close();

        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        out.printf("Found %d target leaf-samples to trace (%d unique timestamp|thread keys)%n%n", scanned, keyToLabel.size());

        // Pass 2: collect Stack event frames for those keys.
        Map<String, List<String>> stacksByKey = new LinkedHashMap<>();
        BufferedReader in2 = new BufferedReader(new FileReader(dumpPath));
        while ((line = in2.readLine()) != null) {
            if (!line.trim().startsWith("Stack,")) continue;
            String[] parts = line.split(",");
            if (parts.length < 5) continue;
            String timestamp = parts[1].trim();
            String threadId = parts[2].trim();
            String key = timestamp + "|" + threadId;
            if (!keyToLabel.containsKey(key)) continue;
            String frameNo = parts[3].trim();
            String imageFunc = parts[5].trim();
            stacksByKey.computeIfAbsent(key, k -> new ArrayList<>()).add(frameNo + ":" + imageFunc);
        }
        in2.close();

        // Aggregate: for each label, collect a "signature" of resolved call
        // chains (only the LRFF13.exe frames, resolved to Ghidra functions)
        // and count how often each distinct chain occurs.
        Map<String, Map<String, Integer>> chainCountsByLabel = new LinkedHashMap<>();
        for (Map.Entry<String, String> e : keyToLabel.entrySet()) {
            String key = e.getKey();
            String label = e.getValue();
            List<String> frames = stacksByKey.get(key);
            if (frames == null) continue;
            List<String> resolvedChain = new ArrayList<>();
            for (String fr : frames) {
                String[] fp = fr.split(":", 2);
                String imageFunc = fp.length > 1 ? fp[1] : fr;
                if (imageFunc.startsWith("LRFF13.exe!0x")) {
                    long runtimeAddr;
                    try {
                        runtimeAddr = Long.parseLong(imageFunc.substring("LRFF13.exe!0x".length()), 16);
                    } catch (Exception ex) { continue; }
                    String resolved = resolveGhidra(runtimeAddr - ASLR_OFFSET);
                    resolvedChain.add(resolved);
                } else {
                    resolvedChain.add(imageFunc);
                }
            }
            String chainStr = String.join(" <- ", resolvedChain);
            chainCountsByLabel.computeIfAbsent(label, k -> new HashMap<>()).merge(chainStr, 1, Integer::sum);
        }

        for (Map.Entry<String, Map<String, Integer>> e : chainCountsByLabel.entrySet()) {
            out.printf("=== %s ===%n", e.getKey());
            List<Map.Entry<String, Integer>> chains = new ArrayList<>(e.getValue().entrySet());
            chains.sort((a, b) -> b.getValue() - a.getValue());
            for (Map.Entry<String, Integer> c : chains) {
                out.printf("  [%d samples] %s%n", c.getValue(), c.getKey());
            }
            out.println();
        }

        out.close();
        println("DONE. Written to " + outPath);
    }
}
