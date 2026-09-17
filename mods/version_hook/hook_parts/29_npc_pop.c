// ---- NPC population distances (FieldGlobal overrides) ----------------------
// sys/wdbpack.bin : r_field_global.wdb ("FieldGlobal") is mirrored by the
// engine into a static table of 0x20-byte records at 0x024c8a30 (Ghidra
// base): name[16] at +0, exe default float at +0x10, LIVE float at +0x14,
// int(live) at +0x18. Loader FUN_005b2f60 rewrites +0x14 on every field
// load; the field code reads through FUN_005b2f20 (float, idx) /
// FUN_005b2f40 (int, idx). Recon and index map: POST_BETA_PLAN.md "Next
// graphics round" C, tools/ghidra_scripts/PopSystem.java.
//
//   idx 37 POPPopLength    80   NPC pop-in distance (world units)
//   idx 38 POPDepopLength  100  NPC despawn distance
//
// The override is applied on the monitor cadence, after the loader has had
// its turn, and is SELF-VALIDATING: the record's name field is compared to
// the expected string before anything is written, so a wrong base or a
// different exe build degrades to "no effect + one log line", never to a
// blind poke. 0 = engine value. Both are written as float AND as the int
// mirror at +0x18, since which one the consumer reads is not known.
#define FIELDGLOBAL_TABLE_RVA (0x024c8a30 - 0x00400000)
#define FG_IDX_POP_LENGTH   37
#define FG_IDX_DEPOP_LENGTH 38
#define FG_IDX_NEARLEN_MOB  39      // POPWNearLenMob, 150: the mob pop window

// g_npcPopWrites lives in 03_render_state.c (status line in 18 reads it).
static LONG g_npcLogged = 0;

static float *FgLiveSlot(int idx, const char *expectName)
{
    unsigned char *rec = (unsigned char *)g_mainModBase + FIELDGLOBAL_TABLE_RVA + idx * 0x20;
    if (strncmp((const char *)rec, expectName, 16) != 0) return NULL;
    return (float *)(rec + 0x14);
}

static void ApplyNpcPopDistances(void)
{
    if (g_mainModBase == 0) return;
    __try {
        float *pop   = FgLiveSlot(FG_IDX_POP_LENGTH,   "POPPopLength");
        float *depop = FgLiveSlot(FG_IDX_DEPOP_LENGTH, "POPDepopLength");
        if (!pop || !depop) {
            if (g_npcLogged == 0) {
                g_npcLogged = 1;
                LogLine("[npc] FieldGlobal table names do not match at the expected slots - overrides disabled");
            }
            return;
        }
        // Table dump: after every field load (the loader rewrites +0x14, so a
        // change in a row we do not write - POPWNearLength - marks one). The
        // first version dumped as soon as OUR write landed, which was before
        // the loader had filled anything else: all zeros, nothing learned.
        {
            static float lastNear = -1.0f;
            float *nearL = FgLiveSlot(33, "POPWNearLength");
            float cur = nearL ? *nearL : -1.0f;
            if (cur != lastNear && cur != 0.0f) { lastNear = cur; g_npcLogged = 0; }
        }
        if (g_npcLogged == 0 && *pop > 0.0f) {
            // Layout proof: every POP*/FE* record's name and live value.
            char l[200];
            g_npcLogged = 1;
            for (int i = 30; i < 111; i++) {
                unsigned char *rec = (unsigned char *)g_mainModBase + FIELDGLOBAL_TABLE_RVA + i * 0x20;
                if (strncmp((const char *)rec, "POP", 3) != 0 && strncmp((const char *)rec, "FEPop", 5) != 0 &&
                    strncmp((const char *)rec, "DZ", 2) != 0) continue;
                sprintf(l, "[npc] FieldGlobal[%d] %-16.16s default=%g live=%g int=%ld",
                        i, (const char *)rec, *(float *)(rec + 0x10), *(float *)(rec + 0x14), *(LONG *)(rec + 0x18));
                LogLine(l);
            }
        }
        // Depop must stay beyond pop (the engine asserts it), so a raised pop
        // distance drags depop along when the user left depop at engine value.
        // Off = stop writing; the loader restores the engine's values on the
        // next field load, and until then the last written value stands (a
        // distance, harmless). Restore immediately from the exe default at
        // +0x10 is NOT done: that slot is the exe fallback, not the WDB value.
        float wantPop   = (g_npcSpawnFix && g_npcPopLength   > 0) ? (float)g_npcPopLength   : 0.0f;
        float wantDepop = (g_npcSpawnFix && g_npcDepopLength > 0) ? (float)g_npcDepopLength : 0.0f;
        if (wantPop > 0.0f && wantDepop <= 0.0f && *depop < wantPop + 20.0f) wantDepop = wantPop + 20.0f;
        if (wantPop > 0.0f && *pop != wantPop) {
            *pop = wantPop; *(LONG *)((unsigned char *)pop + 4) = (LONG)wantPop;
            InterlockedIncrement(&g_npcPopWrites);
        }
        if (wantDepop > 0.0f && *depop != wantDepop) {
            *depop = wantDepop; *(LONG *)((unsigned char *)depop + 4) = (LONG)wantDepop;
            InterlockedIncrement(&g_npcPopWrites);
        }
        // Mob NPCs have their own window radius (POPWNearLenMob, 150). A pop
        // distance beyond it moves the placed NPCs out but leaves the random
        // ones gated at 150 - the "diluted" look. Follow the pop distance.
        {
            float *nearMob = FgLiveSlot(FG_IDX_NEARLEN_MOB, "POPWNearLenMob");
            float wantMob = !g_npcSpawnFix ? 0.0f : g_npcMobWindow > 0 ? (float)g_npcMobWindow
                          : (wantPop > 0.0f && wantPop + 50.0f > 150.0f) ? wantPop + 50.0f : 0.0f;
            if (nearMob && wantMob > 0.0f && *nearMob != wantMob) {
                *nearMob = wantMob; *(LONG *)((unsigned char *)nearMob + 4) = (LONG)wantMob;
                InterlockedIncrement(&g_npcPopWrites);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---- NPC manager pool immediates ----------------------------------------
// FUN_005a02f0 (the field NPC manager update, ~4 KB, called per frame)
// builds a parameter block for the pop-window scheduler (FUN_0049a310)
// with hardcoded capacities next to the POPW* FieldGlobal values:
//
//   005a0719  B8 80 00 00 00        mov eax, 0x80    -> block[-0x4c], [-0x48]
//   005a072f  C7 45 C8 14 00 00 00  mov [ebp-0x38], 0x14
//   005a0739  push 0x24 / call getI -> POPSuggBordar (1400)
//   005a0741  B8 30 00 00 00        mov eax, 0x30    -> block[-0x24], [-0x20]
//
// with slot-array sizes 0x117/0x118/0x127 (279/280/295) in the same block
// and a 296-wide update loop. Which of 128 / 48 / 20 is the mob NPC cap is
// established by trying them (pop distance 80->200 thinned the random NPCs,
// which is what a fixed count over 6x the area looks like). Each is a
// single-dword immediate rewrite, verified against the original bytes first,
// restorable, applied live on the monitor cadence.
static unsigned char *g_npcPoolSiteA = NULL, *g_npcPoolSiteB = NULL, *g_npcPoolSiteC = NULL;
static LONG g_npcPoolPatched = 0;

static unsigned char *NpcPoolSite(unsigned char *base, unsigned rva, const unsigned char *expect, int n)
{
    unsigned char *site = base + rva;
    return memcmp(site, expect, n) == 0 ? site : NULL;
}

static void InstallNpcPoolPatch(unsigned char *base)
{
    static const unsigned char eA[5] = { 0xB8, 0x80, 0x00, 0x00, 0x00 };
    static const unsigned char eC[7] = { 0xC7, 0x45, 0xC8, 0x14, 0x00, 0x00, 0x00 };
    static const unsigned char eB[5] = { 0xB8, 0x30, 0x00, 0x00, 0x00 };
    g_npcPoolSiteA = NpcPoolSite(base, 0x005a0719 - 0x00400000, eA, 5);
    g_npcPoolSiteC = NpcPoolSite(base, 0x005a072f - 0x00400000, eC, 7);
    g_npcPoolSiteB = NpcPoolSite(base, 0x005a0741 - 0x00400000, eB, 5);
    if (!g_npcPoolSiteA || !g_npcPoolSiteB || !g_npcPoolSiteC) {
        LogLine("[npc] pool immediates not where expected - NpcPool* disabled");
        HookRegNote("NPC pool immediates", 0);
        g_npcPoolSiteA = g_npcPoolSiteB = g_npcPoolSiteC = NULL;
        return;
    }
    g_npcPoolPatched = 1;
    HookRegNote("NPC pool immediates", 1);
    LogLine("[npc] pool immediates located (A=0x80 pair, B=0x30 pair, C=0x14)");
}

static void NpcPoolWrite(unsigned char *site, int immOff, DWORD want, DWORD engine, const char *tag)
{
    DWORD cur = *(DWORD *)(site + immOff);
    DWORD v = want ? want : engine;
    DWORD oldProtect;
    if (cur == v) return;
    if (!VirtualProtect(site + immOff, 4, CODE_PAGE_WRITABLE, &oldProtect)) return;
    *(DWORD *)(site + immOff) = v;
    VirtualProtect(site + immOff, 4, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), site, 8);
    {
        char l[96];
        sprintf(l, "[npc] pool %s: %lu -> %lu", tag, cur, v);
        LogLine(l);
    }
}

// Category caps, from the manager's accounting loop (FUN_005a02f0, read
// 2026-09-15): per frame, each category's cap is decremented as candidates
// are granted "active"; no array is sized by it, so the only hard limit
// is the 296 entity slots the field shares. Category 1's threshold is
// POPSuggBordar against a score built from POPMobBase -> category 1 (cap
// 0x14 = 20, C) is the mob NPCs; A (128) placed NPCs, B (48) probably
// enemies.
// Monitor cadence, after ApplyNpcPopDistances.
static void ApplyNpcPools(void)
{
    if (!g_npcPoolPatched) return;
    __try {
        int on = g_npcSpawnFix != 0;
        NpcPoolWrite(g_npcPoolSiteA, 1, on ? (DWORD)g_npcPoolA : 0, 0x80, "A");
        NpcPoolWrite(g_npcPoolSiteB, 1, on ? (DWORD)g_npcPoolB : 0, 0x30, "B");
        NpcPoolWrite(g_npcPoolSiteC, 3, on ? (DWORD)g_npcPoolC : 0, 0x14, "C");
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
