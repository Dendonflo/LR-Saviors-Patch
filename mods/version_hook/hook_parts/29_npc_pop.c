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
// The loader's own values for the three rows we write, latched each time it
// runs (it rewrites +0x14 on every field load, before our first write lands
// on the monitor tick). These are what the governor falls back to; the WDB
// numbers (80 / 100 / 150) stand in until the first field load.
static float g_npcEnginePop = 80.0f, g_npcEngineDepop = 100.0f, g_npcEngineMob = 150.0f;

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
            float *nearMob = FgLiveSlot(FG_IDX_NEARLEN_MOB, "POPWNearLenMob");
            float cur = nearL ? *nearL : -1.0f;
            if (cur != lastNear && cur != 0.0f) {
                lastNear = cur; g_npcLogged = 0;
                // The loader has just run: the rows hold ITS values right now.
                if (*pop > 0.0f && *depop > *pop) { g_npcEnginePop = *pop; g_npcEngineDepop = *depop; }
                if (nearMob && *nearMob > 0.0f) g_npcEngineMob = *nearMob;
            }
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
        // Governor fallback: hold the rows at the loader's own values - the
        // engine's behaviour exactly, far NPCs step out, nothing within the
        // engine's own range is touched. Written explicitly (not "stop
        // writing"), because our last value would otherwise stand until the
        // next field load.
        int vanilla = g_npcSpawnFix && g_npcGovVanillaDist;
        float wantPop   = vanilla ? g_npcEnginePop   : (g_npcSpawnFix && g_npcPopLength   > 0) ? (float)g_npcPopLength   : 0.0f;
        float wantDepop = vanilla ? g_npcEngineDepop : (g_npcSpawnFix && g_npcDepopLength > 0) ? (float)g_npcDepopLength : 0.0f;
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
            float wantMob = !g_npcSpawnFix ? 0.0f : vanilla ? g_npcEngineMob
                          : g_npcMobWindow > 0 ? (float)g_npcMobWindow
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

// Two writers share these sites since the governor: the monitor thread
// (ApplyNpcPools, 500 ms) and the main thread (NpcActorGovernorTick, per
// frame). Both toggle the SAME page's protection, so without a lock one
// thread's restore can land between the other's unprotect and its store -
// an access violation inside our own DLL. Short spin, the section is a
// few hundred instructions. tag NULL = silent (the governor's per-frame
// path; its own line reports the cuts).
static volatile LONG g_npcPoolWriteLock = 0;

static void NpcPoolWrite(unsigned char *site, int immOff, DWORD want, DWORD engine, const char *tag)
{
    DWORD v = want ? want : engine;
    DWORD cur, oldProtect;
    if (*(DWORD *)(site + immOff) == v) return;
    while (InterlockedCompareExchange(&g_npcPoolWriteLock, 1, 0) != 0) YieldProcessor();
    cur = *(DWORD *)(site + immOff);
    if (cur != v && VirtualProtect(site + immOff, 4, CODE_PAGE_WRITABLE, &oldProtect)) {
        *(DWORD *)(site + immOff) = v;
        VirtualProtect(site + immOff, 4, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), site, 8);
    } else {
        cur = v;
    }
    InterlockedExchange(&g_npcPoolWriteLock, 0);
    if (cur != v && tag) {
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
// The mob cap written is the EFFECTIVE one (g_npcPoolCEff), which the
// governor below may hold under the configured value; 0 there means
// "follow the setting". Off = engine values, governor idle.
static void ApplyNpcPools(void)
{
    if (!g_npcPoolPatched) return;
    __try {
        int on = g_npcSpawnFix != 0;
        LONG c = g_npcPoolCEff ? g_npcPoolCEff : g_npcPoolC;
        NpcPoolWrite(g_npcPoolSiteA, 1, on ? (DWORD)g_npcPoolA : 0, 0x80, "A");
        NpcPoolWrite(g_npcPoolSiteB, 1, on ? (DWORD)g_npcPoolB : 0, 0x30, "B");
        NpcPoolWrite(g_npcPoolSiteC, 3, on ? (DWORD)c : 0, 0x14, "C");
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- SceneActor budget governor ------------------------------------------
// The hard limit the caps above were believed not to have (POST_BETA_PLAN
// "no array is sized by a cap") exists one layer down: the scene object
// manager reserves 144 SceneActors (Scene.cpp, table DAT_0208ff74 = {8,
// 144, 48, 4, 1, 48} per object type, type 1 = actor) out of a 256-entry
// handle table shared by every type, so it cannot simply be raised. The
// actors live in a GfMemBlockBuffer at DAT_024d1768: +4 capacity, +8
// element size (0xa50), +0xc/+0x10 range, +0x14 free-list head, +0x18 FREE
// COUNT. SceneObjectManager::Create (FUN_006664b0) returns handle -1 when
// the pool is empty (a dev-build warning, silent in retail) and nothing on
// the field side checks: the chara is activated anyway, its actor handle
// resolves to the manager's placeholder SceneObject, AsActor() on that is
// NULL, and the AI activate (AIcomG_AreaRdm, FUN_005efb90) reads the name
// at NULL+0x348. That is the Yusnaan crash of 2026-09-18 (CRASHES.md),
// reached with the shipped Extended preset (mob cap 80 + pop 200) in the
// Reveler's Quarter crowd once two Flanitors turned hostile.
//
// Engine defaults (128 placed + 20 mob + 48 enemies) already exceed 144 on
// paper; the levels were authored so it never happens at pop 80. Extended
// breaks that assumption, so this keeps it by feedback instead, with
// VANILLA AS THE FLOOR at every stage - the governor can only take back
// what Extended added, never less than the engine's own behaviour, so an
// NPC vanilla would show is never held back:
//
//   stage 1  the mob cap is cut by the deficit, down to the engine's 20.
//            The manager deactivates the lowest-priority actives beyond a
//            cap on its next update (FUN_005a02f0's grant loop counts the
//            cap down over kept + new). Mob = category 1, the random
//            walkers (AIcomG_AreaRdm); placed NPCs are category 0 and are
//            NOT capped by this - their order within their own category is
//            the engine's, unverified, so their cap is left alone.
//   stage 2  still short with the mob cap at 20: the pop / depop / mob
//            window rows go back to the loader's own values (80/100/150),
//            so the far NPCs that only exist because of Extended step out.
//            Anything within the engine's own range is untouched.
//
// Above reserve + 8, stage 2 lifts first (the placed NPCs are the authored
// ones), then the mob cap is raised by the SURPLUS (free - reserve - 8)
// once per 15 frames: every walker admitted takes one actor, so the raise
// cannot push free under the reserve by itself, and a jump straight to the
// setting would (the manager admits everything the cap allows on its next
// update, then the cut fires - visible pop-in/pop-out). The 15-frame
// spacing covers activations whose model is still streaming and take
// their actor a few frames late. A zone load transient (run 4, 2026-09-18:
// old and new area actors alive together, free 11, cap driven to 20)
// recovers in about a second instead of the 15 s a one-per-step ramp
// took. Cuts are
// rate-limited to one per 20 frames because a deactivation frees its actor
// a little later, not the same frame - except under half the reserve,
// where the next frame cuts again: the 2026-09-18 run 2 saw an aggro burst
// take ~20 actors inside 500 ms and the timer alone let free fall to 8 of
// a 16 reserve. Self-validating: element size and
// capacity are checked before the pool is trusted, and a mismatch leaves
// everything alone.
#define SCENE_ACTOR_POOL_RVA  (0x024d1768 - 0x00400000)
#define SCENE_ACTOR_ELEM_SIZE 0xa50
#define NPC_GOV_FLOOR_C       20      // engine mob cap
#define NPC_GOV_HYSTERESIS    8
#define NPC_GOV_CUT_FRAMES    20
#define NPC_GOV_RAMP_FRAMES   15

static int NpcActorPoolRead(LONG *freeOut, LONG *countOut)
{
    if (g_mainModBase == 0) return 0;
    __try {
        unsigned char *pool = *(unsigned char **)((unsigned char *)g_mainModBase + SCENE_ACTOR_POOL_RVA);
        if (!pool) return 0;
        {
            LONG count = *(LONG *)(pool + 0x4);
            LONG freeN = *(LONG *)(pool + 0x18);
            DWORD elem = *(DWORD *)(pool + 0x8) & 0x7fffffff;
            if (elem != SCENE_ACTOR_ELEM_SIZE || count <= 0 || count > 4096 || freeN < 0 || freeN > count)
                return 0;
            *freeOut = freeN;
            *countOut = count;
            return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void NpcActorGovernorTick(void)
{
    static LONG sinceCut = 0, sinceRamp = 0;
    LONG freeN, count;
    LONG reserve = g_npcActorReserve;
    LONG wantC = g_npcPoolC ? g_npcPoolC : NPC_GOV_FLOOR_C;
    LONG floorC = wantC < NPC_GOV_FLOOR_C ? wantC : NPC_GOV_FLOOR_C;
    LONG effC, vanilla, wasVanilla;
    if (!NpcActorPoolRead(&freeN, &count)) { g_npcActorsFree = -1; return; }
    g_npcActorsFree = freeN;
    g_npcActorsCount = count;
    if (g_npcActorsMinFree < 0 || freeN < g_npcActorsMinFree) g_npcActorsMinFree = freeN;
    if (!g_npcPoolPatched || !g_npcSpawnFix || reserve <= 0) {
        g_npcPoolCEff = 0;
        if (g_npcGovVanillaDist) { g_npcGovVanillaDist = 0; ApplyNpcPopDistances(); }
        return;
    }
    effC = g_npcPoolCEff ? g_npcPoolCEff : wantC;
    if (effC > wantC) effC = wantC;      // setting lowered under us
    vanilla = wasVanilla = g_npcGovVanillaDist;
    sinceCut++; sinceRamp++;
    if (freeN < reserve) {
        if (sinceCut >= NPC_GOV_CUT_FRAMES || freeN < reserve / 2) {
            LONG deficit = reserve - freeN;
            LONG cutC = effC - floorC; if (cutC > deficit) cutC = deficit; if (cutC < 0) cutC = 0;
            effC -= cutC;
            if (cutC == 0 && effC <= floorC) vanilla = 1;   // stage 2
            if (cutC || vanilla != wasVanilla) {
                char l[160];
                InterlockedIncrement(&g_npcGovCuts);
                sprintf(l, "[npc] actors free=%ld/%ld under reserve %ld - mob cap %ld%s",
                        freeN, count, reserve, effC, vanilla ? ", distances at engine values" : "");
                LogLine(l);
            }
            sinceCut = 0;
        }
    } else if (freeN > reserve + NPC_GOV_HYSTERESIS && sinceRamp >= NPC_GOV_RAMP_FRAMES) {
        // Distances first (the placed NPCs are the authored ones), mobs after.
        LONG surplus = freeN - reserve - NPC_GOV_HYSTERESIS;
        if (vanilla) vanilla = 0;
        else if (effC < wantC) { effC += surplus; if (effC > wantC) effC = wantC; }
        sinceRamp = 0;
    }
    g_npcPoolCEff = (effC == wantC) ? 0 : effC;
    g_npcGovVanillaDist = vanilla;
    // Apply here too: the monitor's 500 ms cadence is 30 frames of new
    // activations at 60 fps, long enough to empty the reserve.
    __try {
        NpcPoolWrite(g_npcPoolSiteC, 3, (DWORD)effC, 0x14, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (vanilla != wasVanilla) {
        ApplyNpcPopDistances();
        if (!vanilla) LogLine("[npc] actors recovered - extended distances resumed");
    }
}
