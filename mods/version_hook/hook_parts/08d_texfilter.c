// ---- Texture filtering: state, census and report --------------------------
//
// THE FACT THIS IS BUILT ON (static, verified against the shipped exe):
// the game's Graphics > Texture Filtering menu has exactly two entries, and
// both are three-line functions writing one integer into the engine's
// settings struct:
//
//     Graphics_TextureFiltering_Advanced  FUN_00acad50   settings+0x38 = 8
//     Graphics_TextureFiltering_Standard  FUN_00acad80   settings+0x38 = 1
//
// (settings base = the pointer at DAT_0511558c; disassembled 2026-08-19, both
// handlers are "if (apply) field = N; return field == N;" and nothing else.)
// So the ceiling really is 8x, it is a plain DWORD, and "Standard" is 1x -
// i.e. trilinear only. That is the whole vanilla range, which is what
// motivated replacing the pair with Off / 2x / 4x / 8x / 16x.
//
// WHY THE OVERRIDE IS NOT "WRITE 16 INTO THAT FIELD". Nothing else in the
// exe reads +0x38 through that global - a scan of all 138 xrefs to it found
// only these two handlers - so the consumer reads it through some copy we
// have not identified, and we would be feeding an unknown reader a value its
// own menu can never produce. It is also the field the engine SERIALISES to
// Configuration.ini, and an out-of-range enum there is a plausible way to
// trip the settings reset this project has already been bitten by. So the
// engine field is left holding a value the engine itself can produce, and
// the real enforcement happens one layer down, at D3D, where the value is
// unambiguous and every rewrite is counted.
//
// WHAT THE HOOK DOES (12b_texfilter_hook.c). SetSamplerState is the only
// place a D3D9 engine can express texture filtering, so hooking it makes our
// level authoritative regardless of how the engine arrived at its own. Two
// rewrites:
//
//   - MAXANISOTROPY -> our level, and MINFILTER LINEAR -> ANISOTROPIC, on
//     stages that qualify. This is the feature.
//   - MIPFILTER POINT -> LINEAR, gated separately (ForceTrilinear). POINT
//     mip filtering is the classic visible-mip-band defect; whether this
//     game actually does it is what the census below exists to answer, so
//     the rewrite ships OFF until the census says it is needed.
//
// THE QUALIFYING RULE is the safety of the whole thing, so it is derived
// from the engine's own intent rather than from a list of stage numbers:
// a stage qualifies once the engine has enabled MIPMAPPING on it. Anisotropic
// filtering is a refinement of mip selection, so a sampler with no mip chain
// gains exactly nothing from it - and the samplers that must NOT be touched
// (fullscreen post passes, colour LUTs, shadow-map comparisons, our own UI
// blit) are precisely the ones an engine leaves at MIPFILTER NONE. A stage
// list would rot the first time the engine bound something differently;
// this cannot.
//
// SPLIT ACROSS TWO FILES for one mechanical reason: d3d9.h does not enter
// this translation unit until 12_locks_staging.c, so everything naming a D3D
// type lives in 12b_texfilter_hook.c. This file holds what 09/10/11 need -
// the state, the counters and the report - and names no D3D type.

#define TF_STAGES     16
#define TF_STATES     14      // D3DSAMP_* run 1..13

// D3DTEXTUREFILTERTYPE, spelled out because d3d9.h is not in the TU yet here.
// Used as histogram indices below; 12b uses the real names.
#define TF_F_NONE     0
#define TF_F_POINT    1
#define TF_F_LINEAR   2
#define TF_F_ANISO    3

// The engine's last-written value per stage, and whether it ever wrote one.
// Flat arrays rather than a struct: the hot path indexes by the sampler-state
// enum directly, and this is called several thousand times a frame.
static DWORD         g_tfEng[TF_STAGES][TF_STATES];
static unsigned char g_tfEngKnown[TF_STAGES][TF_STATES];
// What the DEVICE currently holds for MAXANISOTROPY - NOT the same as what
// the engine asked for, because we push that value onto stages the engine
// never sets it on at all (its default is 1, which would silently defeat an
// upgraded MINFILTER).
static DWORD         g_tfDevAniso[TF_STAGES];

static volatile LONG g_tfCalls      = 0;   // engine sampler-state writes seen
static volatile LONG g_tfMinUp      = 0;   // MINFILTER  linear -> anisotropic
static volatile LONG g_tfAnisoSet   = 0;   // MAXANISOTROPY rewritten or pushed
static volatile LONG g_tfMipUp      = 0;   // MIPFILTER  point  -> linear
static volatile LONG g_tfStagesSeen = 0;   // bitmask of stages the engine uses
static volatile LONG g_tfMipStages  = 0;   // bitmask of stages with mips on
static volatile LONG g_tfBiasWrites = 0;   // non-zero MIPMAPLODBIAS writes
static LONG          g_tfBiasLast   = 0;   // float bits of the last such value
// LOD bias, broken out (2026-08-19). The first gameplay run showed the engine
// writing this ~890k times with values on BOTH sides of zero - so "does it
// touch the bias" is answered and the useful question is now which way and
// how far. A POSITIVE bias selects a blurrier mip than the pixel footprint
// calls for, which is one of the two ways to get "mushy textures" that no
// filtering setting can fix; a negative one sharpens and buys shimmer. Only
// the positive side is a candidate defect, so it is counted separately, and
// the stage mask says whether it lands on many samplers or one special case.
static volatile LONG g_tfBiasPos = 0, g_tfBiasNeg = 0;
static volatile LONG g_tfBiasClamped = 0;   // negative biases we floored
static volatile LONG g_tfBiasPosStages = 0;
static LONG          g_tfBiasMinBits = 0, g_tfBiasMaxBits = 0;
static volatile LONG g_tfMaxLevelNZ = 0;   // non-zero MAXMIPLEVEL writes
static volatile LONG g_tfCapAniso   = 0;   // D3DCAPS9.MaxAnisotropy, 0 = unread
static volatile LONG g_tfRepush     = 0;   // level changed: re-push every stage

// Value histograms across all stages. Filter enums are 0..7 (NONE, POINT,
// LINEAR, ANISOTROPIC, -, -, PYRAMIDALQUAD, GAUSSIANQUAD).
static volatile LONG g_tfMinHist[8], g_tfMagHist[8], g_tfMipHist[8];
static volatile LONG g_tfAnisoHist[6];     // 1 / 2 / 4 / 8 / 16 / other

// Mip-chain census, fed from HookedCreateTexture via 12b. The other half of
// the "mipmapping issues" question: a large surface texture created with
// exactly ONE level has no mip chain at all, so no filtering setting can stop
// it shimmering - that is a content/loader defect, not a sampler defect, and
// the two are indistinguishable by eye. Levels==0 means "build the full
// chain", so it counts as healthy.
static volatile LONG g_tfTexTotal = 0, g_tfTexSingle = 0, g_tfTexFull = 0;
// Broken down, because the raw single-level count is NOT self-interpreting.
// A UI atlas, a lookup table and another mod's HD GUI upload are all
// legitimately single-level and all clear 64px; a 512 or 1024 DXT block is a
// world surface and a real defect. Size bucket (64/128/256/512/1024+) and
// compressed-vs-not separate those two populations, and until they are
// separated the count cannot support a conclusion either way.
static volatile LONG g_tfTexSingleBucket[5];
static volatile LONG g_tfTexSingleDxt = 0, g_tfTexSingleRaw = 0;
// WHO created it. The size/format split narrowed the single-level population
// to compressed art, which would be a real defect - but this process also
// hosts the HD GUI mod, whose replacement textures are large, compressed and
// legitimately single-level. Nothing about a texture's shape distinguishes
// those two, and the CALLER does: game-exe creations are the engine loading
// its own assets, anything else is another module. Same idiom as the halfres
// probe in 13.
static volatile LONG g_tfTexSingleGame = 0, g_tfTexSingleForeign = 0;
static volatile LONG g_tfTexDetail = 0;

// Called when the level changes from the menu. Most of the engine's sampler
// state is re-set per draw batch, so a change shows up within a frame on its
// own - but a stage the engine configures ONCE at load would never see it,
// and "the setting did nothing until I reloaded" is exactly the kind of
// report that costs a test round-trip to diagnose. Consumed in
// TexFilterFrameTick (12b).
static void TexFilterMarkDirty(void)
{
    InterlockedExchange(&g_tfRepush, 1);
}

// ---- the report ----------------------------------------------------------
// CADENCE REWRITTEN 2026-08-19, after the first census run produced nothing
// usable. The original emitted whenever the line's TEXT changed, under a cap
// of 24 lines. Both halves were wrong together: the text contains the
// histograms, so it changed on every single tick, and the cap was therefore
// spent at two lines per 500ms - the whole budget gone 13 seconds in, while
// the game was still LOADING. The user then played and toggled the setting
// repeatedly, and none of it was recorded. A change-gate over a line that
// always changes is not a gate.
//
// What replaces it:
//   - a SETTING change always emits, immediately and uncapped. Those are user
//     actions, there are a handful of them per session, and they are the
//     events the whole census exists to bracket.
//   - otherwise a geometric schedule of monitor ticks (they run every 500ms):
//     10s, 30s, 2min, 10min, then every 10min. Bounded at roughly six lines
//     an hour, and unlike the old cap it is still reporting once the player
//     is actually playing - the only window where the answer means anything,
//     since boot-time data describes a loading screen.
//
// The FILTER HISTOGRAMS are now WINDOWED: reset after every emit, so each line
// describes the period since the previous one. Cumulative totals were the
// other half of what made the first run unreadable - a gameplay sample
// diluted into tens of thousands of loading-screen writes says nothing about
// gameplay. The texture census on line B stays cumulative, because it is a
// property of what has been loaded rather than a rate.
static void TexFilterTick(void)
{
    static LONG ticks = 0, lastLvl = -1, lastTri = -1, lastBias = -1, nextSlot = 20;
    char a[416], b[416], c[416];
    LONG lvl = g_anisoLevel, tri = g_forceTrilinear, bias = g_mipBiasMode;
    int settingChanged, due;

    if (!g_tfCalls) return;
    ticks++;
    settingChanged = (lvl != lastLvl || tri != lastTri || bias != lastBias);
    due = (ticks >= nextSlot);
    if (!settingChanged && !due) return;
    lastLvl = lvl; lastTri = tri; lastBias = bias;
    if (due) {
        if      (nextSlot <= 20)  nextSlot = 60;      // 10s  -> 30s
        else if (nextSlot <= 60)  nextSlot = 240;     // 30s  -> 2min
        else if (nextSlot <= 240) nextSlot = 1200;    // 2min -> 10min
        else                      nextSlot += 1200;   // then every 10min
    }

    sprintf(a, "[texfilter] aniso=%s trilinear=%s biasFloor=%s cap=%ldx"
               " | since last: min P/L/A=%ld/%ld/%ld"
               " mag P/L=%ld/%ld mip N/P/L=%ld/%ld/%ld | engine aniso 1/2/4/8/16/other="
               "%ld/%ld/%ld/%ld/%ld/%ld",
            lvl <= 0 ? "engine" : (lvl == 1 ? "off" : (lvl == 2 ? "2x" :
                (lvl == 4 ? "4x" : (lvl == 8 ? "8x" : "16x")))),
            g_forceTrilinear ? "forced" : "engine",
            g_mipBiasMode == 0 ? "engine" : (g_mipBiasMode == 1 ? "0.0" :
                (g_mipBiasMode == 2 ? "-0.5" : "-1.0")),
            g_tfCapAniso,
            g_tfMinHist[TF_F_POINT], g_tfMinHist[TF_F_LINEAR], g_tfMinHist[TF_F_ANISO],
            g_tfMagHist[TF_F_POINT], g_tfMagHist[TF_F_LINEAR],
            g_tfMipHist[TF_F_NONE], g_tfMipHist[TF_F_POINT], g_tfMipHist[TF_F_LINEAR],
            g_tfAnisoHist[0], g_tfAnisoHist[1], g_tfAnisoHist[2],
            g_tfAnisoHist[3], g_tfAnisoHist[4], g_tfAnisoHist[5]);

    {
        float bmin = 0.0f, bmax = 0.0f;
        memcpy(&bmin, &g_tfBiasMinBits, sizeof(bmin));
        memcpy(&bmax, &g_tfBiasMaxBits, sizeof(bmax));
        sprintf(b, "[texfilter] rewrote min=%ld aniso=%ld mip=%ld of %ld writes |"
                   " stages used=0x%04lX mipped=0x%04lX | lodBias +/-=%ld/%ld"
                   " range %.2f..%.2f blurStages=0x%04lX clamped=%ld | maxMipLevel nz=%ld",
                g_tfMinUp, g_tfAnisoSet, g_tfMipUp, g_tfCalls,
                (unsigned long)(g_tfStagesSeen & 0xFFFF),
                (unsigned long)(g_tfMipStages & 0xFFFF),
                g_tfBiasPos, g_tfBiasNeg, (double)bmin, (double)bmax,
                (unsigned long)(g_tfBiasPosStages & 0xFFFF), g_tfBiasClamped,
                g_tfMaxLevelNZ);
    }
    sprintf(c, "[texfilter] 1-level tex %ld of %ld:"
               " 64/128/256/512/1k+=%ld/%ld/%ld/%ld/%ld dxt=%ld raw=%ld"
               " | creator game=%ld other=%ld",
            g_tfTexSingle, g_tfTexTotal,
            g_tfTexSingleBucket[0], g_tfTexSingleBucket[1], g_tfTexSingleBucket[2],
            g_tfTexSingleBucket[3], g_tfTexSingleBucket[4],
            g_tfTexSingleDxt, g_tfTexSingleRaw,
            g_tfTexSingleGame, g_tfTexSingleForeign);

    LogLine(a);
    LogLine(b);
    LogLine(c);
    // Window reset - see the cadence block. Filter histograms only; the
    // rewrite counters and the texture census are session totals.
    memset((void *)g_tfMinHist, 0, sizeof(g_tfMinHist));
    memset((void *)g_tfMagHist, 0, sizeof(g_tfMagHist));
    memset((void *)g_tfMipHist, 0, sizeof(g_tfMipHist));
    memset((void *)g_tfAnisoHist, 0, sizeof(g_tfAnisoHist));
}
