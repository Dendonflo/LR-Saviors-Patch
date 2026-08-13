import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.Map;

// Resolves every leaf-address hit from the combined native-D3D9 capture
// (event + baseline raw hit-count files) to its containing function, to
// precisely determine whether the CharacterSkeletonNode CONSTRUCTION chain
// (FUN_00aaa510/FUN_00aaca20/FUN_00aad000/FUN_00aae210) is actually executing
// during the stutter, as opposed to only the already-confirmed per-frame
// bone-transform chain (FUN_00aac130/FUN_00aacf10). ASLR offset for this
// specific capture session: ghidra_addr = runtime_addr + 0x2B0000.
public class ResolveConstructionVsHot extends GhidraScript {
    static final long ASLR_OFFSET = 0x2B0000L;

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/construction_vs_hot_resolved.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        String[] files = {
            "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/hot_addresses_combined_event_raw.txt",
            "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/hot_addresses_combined_baseline_raw.txt",
            "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/dxvk_event_for_resolve.txt",
            "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/dxvk_baseline_for_resolve.txt"
        };
        String[] labels = {"EVENT", "BASELINE", "DXVK_EVENT", "DXVK_BASELINE"};

        for (int fi = 0; fi < files.length; fi++) {
            Map<String, Integer> perFunc = new LinkedHashMap<>();
            int total = 0;
            BufferedReader br = new BufferedReader(new FileReader(files[fi]));
            String line;
            while ((line = br.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) continue;
                String[] parts = line.split("\\s+");
                if (parts.length < 2) continue;
                int count;
                long runtimeAddr;
                try {
                    count = Integer.parseInt(parts[0]);
                    runtimeAddr = Long.parseLong(parts[1], 16);
                } catch (NumberFormatException e) { continue; }
                long ghidraAddr = runtimeAddr + ASLR_OFFSET;
                Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(ghidraAddr);
                Function func = getFunctionContaining(addr);
                String key = func != null ? (func.getName() + " @ " + func.getEntryPoint()) : ("UNRESOLVED " + Long.toHexString(ghidraAddr));
                perFunc.merge(key, count, Integer::sum);
                total += count;
            }
            br.close();

            out.printf("=== %s (total=%d) ===%n", labels[fi], total);
            perFunc.entrySet().stream()
                .sorted((a, b) -> b.getValue() - a.getValue())
                .forEach(e -> out.printf("%6d  %s%n", e.getValue(), e.getKey()));
            out.println();
        }

        out.close();
        println("DONE");
    }
}
