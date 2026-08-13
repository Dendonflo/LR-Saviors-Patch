// Determine whether BgLoader::load_block (and its call chain) executes on
// the main/render thread or a dedicated worker thread. Strategy:
//  1) Walk callers of key loader entry points up to N levels (a poor-man's
//     call-graph BFS upward) so we can see the full chain back to whatever
//     invokes it.
//  2) Find every CreateThread/_beginthreadex/_beginthread call site and
//     print the function it lives in plus (if a simple immediate/known
//     address) the thread-start routine passed to it.
//  3) Find call sites of common main-loop markers (PeekMessageA/W,
//     DispatchMessageA/W, IDirect3DDevice9::Present-style EndScene/Present
//     imports if present, timeGetTime usage density) so we have a candidate
//     "main loop" function to cross-reference against the upward BFS results.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;

import java.io.PrintWriter;
import java.util.ArrayDeque;
import java.util.Deque;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.Set;

public class TraceLoaderThreadContext extends GhidraScript {

    static final String[] ROOTS = {"00496070", "00496f20", "00494f40"};
    static final int MAX_DEPTH = 6;

    static final String[] THREAD_API_NAMES = {
        "CreateThread", "_beginthreadex", "_beginthread", "SetThreadPriority", "SetThreadAffinityMask"
    };

    static final String[] MAINLOOP_MARKER_NAMES = {
        "PeekMessageA", "PeekMessageW", "DispatchMessageA", "DispatchMessageW",
        "Present", "EndScene", "GetMessageA", "GetMessageW"
    };

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/tools/ghidra_output/thread_context.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        out.println("=== PART 1: upward caller BFS from loader entry points ===");
        for (String rootAddrStr : ROOTS) {
            out.printf("%n--- BFS from %s ---%n", rootAddrStr);
            Address rootAddr = currentProgram.getAddressFactory().getAddress(rootAddrStr);
            Function rootFunc = getFunctionContaining(rootAddr);
            if (rootFunc == null) {
                out.println("    NO FUNCTION FOUND");
                continue;
            }

            Set<String> visited = new HashSet<>();
            Deque<Function> frontier = new ArrayDeque<>();
            frontier.add(rootFunc);
            visited.add(rootFunc.getEntryPoint().toString());
            int depth = 0;

            while (!frontier.isEmpty() && depth < MAX_DEPTH) {
                int levelSize = frontier.size();
                out.printf("  [depth %d] %d function(s)%n", depth, levelSize);
                Deque<Function> nextFrontier = new ArrayDeque<>();
                for (int i = 0; i < levelSize; i++) {
                    Function f = frontier.poll();
                    out.printf("    %s @ %s%n", f.getName(), f.getEntryPoint());
                    Reference[] refs = getReferencesTo(f.getEntryPoint());
                    Set<String> callerNames = new LinkedHashSet<>();
                    for (Reference ref : refs) {
                        Function callerFunc = getFunctionContaining(ref.getFromAddress());
                        if (callerFunc == null) continue;
                        String key = callerFunc.getEntryPoint().toString();
                        if (visited.contains(key)) continue;
                        if (callerNames.add(key)) {
                            visited.add(key);
                            nextFrontier.add(callerFunc);
                        }
                    }
                }
                frontier = nextFrontier;
                depth++;
            }
            if (!frontier.isEmpty()) {
                out.printf("  (BFS truncated at depth %d; %d more caller(s) not expanded)%n", MAX_DEPTH, frontier.size());
            }
        }

        out.println();
        out.println("=== PART 2: thread-creation / priority API call sites ===");
        SymbolTable st = currentProgram.getSymbolTable();
        for (String apiName : THREAD_API_NAMES) {
            SymbolIterator symIter = st.getSymbols(apiName);
            while (symIter.hasNext()) {
                Symbol sym = symIter.next();
                out.printf("API: %s @ %s%n", apiName, sym.getAddress());
                Reference[] refs = getReferencesTo(sym.getAddress());
                for (Reference ref : refs) {
                    Address fromAddr = ref.getFromAddress();
                    Function callerFunc = getFunctionContaining(fromAddr);
                    String callerName = callerFunc != null ? callerFunc.getName() + " @ " + callerFunc.getEntryPoint() : "addr " + fromAddr;
                    out.printf("    called from %s in %s%n", fromAddr, callerName);
                }
            }
        }

        out.println();
        out.println("=== PART 3: main-loop marker API call sites ===");
        for (String apiName : MAINLOOP_MARKER_NAMES) {
            SymbolIterator symIter = st.getSymbols(apiName);
            while (symIter.hasNext()) {
                Symbol sym = symIter.next();
                out.printf("API: %s @ %s%n", apiName, sym.getAddress());
                Reference[] refs = getReferencesTo(sym.getAddress());
                for (Reference ref : refs) {
                    Address fromAddr = ref.getFromAddress();
                    Function callerFunc = getFunctionContaining(fromAddr);
                    String callerName = callerFunc != null ? callerFunc.getName() + " @ " + callerFunc.getEntryPoint() : "addr " + fromAddr;
                    out.printf("    called from %s in %s%n", fromAddr, callerName);
                }
            }
        }

        out.close();
        println("DONE. Written to " + outPath);
    }
}
