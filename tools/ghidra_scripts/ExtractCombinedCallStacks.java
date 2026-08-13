// Same call-stack extraction approach as ExtractHotCallStacks/
// ResolveStacksDiffAndTrace, but for the ground-truth-confirmed combined
// capture. ASLR adjust for this session: ghidra_addr = runtime_addr +
// 0x2B0000 (module base 0x150000, confirmed via live Toolhelp32 read).
// Checking specifically whether the top hot functions (quaternion-to-matrix,
// matrix transpose, bone-transform composer) are reached through the
// parallel job dispatcher (FUN_00ab7810 chain) or called directly on the
// main thread without ever being parallelized -- this determines whether
// the earlier thread-count patch *should* have helped at all.

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

public class ExtractCombinedCallStacks extends GhidraScript {

    static final long ASLR_ADJUST = 0x002B0000L;
    static final int TOP_N = 8;

    String resolveRuntimeAddr(long runtimeAddr) {
        long ghidraAddr = runtimeAddr + ASLR_ADJUST;
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
        String dumpPath = base + "dump_combined_event.csv";
        String outPath = base + "combined_hot_call_stacks.txt";

        // Targets: the top functions from the diff, by Ghidra entry address
        String[][] targets = {
            {"004164f0", "quat_to_matrix"},
            {"0040acb0", "matrix_transpose_util"},
            {"00416a30", "chain_2"},
            {"00437270", "chain_3"},
            {"00aac130", "bone_transform_composer"},
            {"00a29d40", "chain_5"},
            {"00408210", "chain_6"},
            {"00bfb780", "chain_7"}
        };

        Map<String, String> entryToLabel = new LinkedHashMap<>();
        for (String[] t : targets) {
            Address addr = currentProgram.getAddressFactory().getAddress(t[0]);
            Function f = getFunctionContaining(addr);
            if (f == null) continue;
            entryToLabel.put(f.getEntryPoint().toString(), t[1] + "(" + f.getName() + ")");
        }

        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        // Pass 1: find (timestamp|threadid) keys whose leaf resolves to a target
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
            try { runtimeAddr = Long.parseLong(imageFunc.substring("LRFF13.exe!0x".length()), 16); }
            catch (Exception e) { continue; }
            long ghidraAddr = runtimeAddr + ASLR_ADJUST;
            Address addr; Function f;
            try {
                addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(ghidraAddr);
                f = getFunctionContaining(addr);
            } catch (Exception e) { continue; }
            if (f == null) continue;
            String entryStr = f.getEntryPoint().toString();
            if (entryToLabel.containsKey(entryStr)) {
                String timestamp = parts[1].trim();
                String threadId = parts[3].trim();
                keyToLabel.put(timestamp + "|" + threadId, entryToLabel.get(entryStr));
            }
        }
        in1.close();
        out.printf("(%d target leaf-samples matched for stack tracing)%n%n", keyToLabel.size());

        // Pass 2: collect stack frames for those keys
        Map<String, List<String>> stacksByKey = new LinkedHashMap<>();
        BufferedReader in2 = new BufferedReader(new FileReader(dumpPath));
        while ((line = in2.readLine()) != null) {
            String trimmed = line.trim();
            if (!trimmed.startsWith("Stack,")) continue;
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
                    try { runtimeAddr = Long.parseLong(imageFunc.substring("LRFF13.exe!0x".length()), 16); }
                    catch (Exception ex) { continue; }
                    resolvedChain.add(resolveRuntimeAddr(runtimeAddr));
                } else {
                    resolvedChain.add(imageFunc);
                }
            }
            String chainStr = String.join(" <- ", resolvedChain);
            chainCountsByLabel.computeIfAbsent(e.getValue(), k -> new HashMap<>()).merge(chainStr, 1, Integer::sum);
        }

        for (String labelKey : entryToLabel.values()) {
            out.printf("--- %s ---%n", labelKey);
            Map<String, Integer> chains = chainCountsByLabel.get(labelKey);
            if (chains == null || chains.isEmpty()) {
                out.println("  (no stacks captured for this function's samples)");
                out.println();
                continue;
            }
            List<Map.Entry<String, Integer>> sorted = new ArrayList<>(chains.entrySet());
            sorted.sort((a, b) -> b.getValue() - a.getValue());
            int shown = 0;
            for (Map.Entry<String, Integer> c : sorted) {
                out.printf("  [%d] %s%n", c.getValue(), c.getKey());
                if (++shown >= 5) break; // top 5 distinct chains per function is enough
            }
            out.println();
        }

        out.close();
        println("DONE");
    }
}
