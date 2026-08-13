import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;

import java.io.PrintWriter;

// The frame pacer FUN_00ac3040 is confirmed to use a QueryPerformanceCounter
// + Sleep(0/1) spin loop (the pure-timer branch, confirmed active at runtime
// via DAT_0510e964==1). On Windows, Sleep(1) only actually sleeps ~1ms if the
// process has raised the multimedia timer resolution via timeBeginPeriod();
// otherwise it can sleep up to the default tick (historically 15.6ms). A game
// ported from console -- where timers are exact and there is no such API --
// is a classic place for this call to be missing or mismatched.
//
// This searches the whole symbol table + every import for timeBeginPeriod,
// timeEndPeriod, timeGetTime, NtSetTimerResolution, and reports every
// reference site with its containing function, so we can tell (a) whether the
// game ever raises timer resolution at all, and (b) if so, from where.
public class FindTimerResolution extends GhidraScript {
    static final String[] NEEDLES = {
        "timeBeginPeriod", "timeEndPeriod", "timeGetTime",
        "NtSetTimerResolution", "ZwSetTimerResolution",
        "SetWaitableTimer", "CreateWaitableTimer", "timeGetDevCaps"
    };

    @Override
    public void run() throws Exception {
        String outPath = "C:/Users/dendo/Documents/LR FFXIII asset streaming/ghidra_output/timer_resolution_refs.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        SymbolTable st = currentProgram.getSymbolTable();
        for (String needle : NEEDLES) {
            out.printf("=== %s ===%n", needle);
            int hits = 0;
            SymbolIterator it = st.getAllSymbols(true);
            while (it.hasNext()) {
                Symbol s = it.next();
                String nm = s.getName();
                if (nm == null || !nm.toLowerCase().contains(needle.toLowerCase())) continue;
                hits++;
                out.printf("  symbol: %s @ %s (type=%s)%n", nm, s.getAddress(), s.getSymbolType());
                Reference[] refs = getReferencesTo(s.getAddress());
                if (refs.length == 0) {
                    out.println("      (no references to this symbol address)");
                }
                for (Reference r : refs) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    out.printf("      ref from %s (%s) type=%s%n",
                            r.getFromAddress(),
                            f != null ? f.getName() + " @ " + f.getEntryPoint() : "NO FUNC",
                            r.getReferenceType());
                }
            }
            if (hits == 0) out.println("  *** NOT FOUND ANYWHERE IN SYMBOL TABLE ***");
            out.println();
        }

        out.close();
        println("DONE");
    }
}
