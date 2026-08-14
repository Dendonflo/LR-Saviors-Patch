// ---- LR debug menu re-enable (SEPARATE PROJECT - see DEBUG_MENU.md) -------
// The retail exe still contains the entire debug component: 39 DebugMenuPage
// classes, the LoadViewer profiler, the debug terminal, the debug camera and
// the STATE_*_DEBUG states. It is dormant because the enable predicate
// (FUN_00ccea50) is compiled to `return 0`. That predicate is ICF-folded into
// ~1061 vtable slots, so patching its body would flip every "return false"
// virtual in the game - not an option.
//
// The recoverable path is DATA, not code (debug_component_ctor.txt):
//   - boot init FUN_00432200 builds the menu manager singleton
//     DAT_024c3d74 = FUN_004bc200() UNCONDITIONALLY,
//   - sets AppGame+0x658 = 1 UNCONDITIONALLY,
//   - INTERVAL_DEBUG_MENU_WHITE (AppGame vslot 0x164 = FUN_0042a690) ticks
//     the manager every frame,
//   - the ONLY gated boot action is: if (AppGame+0x684 & 2)
//         mgr->byte[6] |= 2|4;   // never fires in retail
// So this file replicates exactly that write on the live object, plus the
// AppGame flag bits the ctor derives from the (stubbed) gate:
//   +0x680 bit1 SET   - the DebugFontTextureDDS ctor (FUN_0042ac60) contains
//                       a null-write assert on this bit being set; if the
//                       font ever constructs with it clear we crash.
//   +0x680 bit2 CLEAR - ctor sets it when the gate returns 0 ("not debug").
//   +0x684 bit1 SET   - the boot-time enable condition above.
//   +0x684 bit 0x40000 CLEAR - ctor sets it when the gate returns 0.
//
// Applied from the monitor thread (500ms), first tick where both singletons
// exist; re-asserted if the engine rewrites the bits (write counter pattern,
// logged only on change - same convention as the SSAA size write).
//
// UNKNOWNS, deliberately left to the in-game test: what input opens the menu
// (DebugMenuHidInterface), and whether sys/debug/DebugFontTextureDDS.bin
// exists in the encrypted archives (loose-file fallback via LayeredFS is
// available if not).

#if ENABLE_DEBUG_MENU

// DAT_024c3d74: debug menu manager singleton, built by FUN_004bc200 at boot.
#define DBGMENU_MGR_PTR_RVA     (0x024c3d74 - 0x00400000)
// DAT_02350974: white::AppGame instance pointer, set in ctor FUN_0042e890.
#define DBGMENU_APPGAME_PTR_RVA (0x02350974 - 0x00400000)

static LONG g_dbgMenuWrites = 0;      // how many times bits had to be applied
static LONG g_dbgMenuLogged = 0;      // one-shot "applied" log latch

static void DebugMenuApply(void)
{
    DWORD appGame, mgr;
    if (!g_mainModBase) return;
    if (g_dbgMenuWrites < 0) return;   // faulted once - stay off for the run
    __try {
        appGame = *(DWORD *)(g_mainModBase + DBGMENU_APPGAME_PTR_RVA);
        mgr     = *(DWORD *)(g_mainModBase + DBGMENU_MGR_PTR_RVA);
        // Both come up during boot init; until then there is nothing to poke.
        // The manager comes up LAST (late in FUN_00432200), so its presence
        // also proves the AppGame flag block is fully initialised - poking
        // the flags any earlier would race the ctor's own writes to them.
        if (!appGame || !mgr) return;
        {
            volatile DWORD *f680 = (volatile DWORD *)(appGame + 0x680);
            volatile DWORD *f684 = (volatile DWORD *)(appGame + 0x684);
            volatile BYTE  *en   = (volatile BYTE  *)(mgr + 6);
            DWORD w680 = (*f680 | 0x2) & ~0x4u;
            DWORD w684 = (*f684 | 0x2) & ~0x40000u;
            BYTE  wen  = (BYTE)(*en | 0x6);
            if (*f680 != w680 || *f684 != w684 || *en != wen) {
                DWORD o680 = *f680, o684 = *f684; BYTE oen = *en;
                *f680 = w680;
                *f684 = w684;
                *en   = wen;
                InterlockedIncrement(&g_dbgMenuWrites);
                // Log the first apply always; later ones only if the engine
                // fought back (bits reverted), which the write counter makes
                // visible without per-tick spam.
                if (!g_dbgMenuLogged || g_dbgMenuWrites <= 3) {
                    char l[192];
                    sprintf(l, "[debugmenu] enable bits applied (#%ld): "
                               "+0x680 %08lx->%08lx  +0x684 %08lx->%08lx  mgr+6 %02x->%02x",
                            g_dbgMenuWrites,
                            (unsigned long)o680, (unsigned long)w680,
                            (unsigned long)o684, (unsigned long)w684,
                            oen, wen);
                    LogLine(l);
                    g_dbgMenuLogged = 1;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Never let an experimental poke take the process down; log once.
        if (!g_dbgMenuLogged) { g_dbgMenuLogged = 1; LogLine("[debugmenu] poke faulted - disabled this run"); }
        g_dbgMenuWrites = -1;
    }
}

#endif  // ENABLE_DEBUG_MENU
