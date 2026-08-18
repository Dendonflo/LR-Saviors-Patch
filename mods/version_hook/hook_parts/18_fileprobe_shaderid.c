// ---- main-thread file-open probe -----------------------------------------
// `ZwCreateFile` (7 records, 0.15s) and `ZwClose` (4, 0.09s) appeared in the
// stutter attribution: the main thread is opening files SYNCHRONOUSLY
// mid-frame. `ReadFile` has been instrumented since this project began;
// `CreateFile` never has, so this has been invisible the entire time. A file
// open is a kernel round-trip that can touch the filesystem (directory walk,
// antivirus filter, cold metadata) and doing it on the render thread is a
// classic hitch source.
//
// The exe imports the ANSI variants (`CreateFileA`, `CloseHandle`) - checked
// against its own import table rather than assumed, since hooking
// `CreateFileW` would have silently done nothing.
//
// Diagnostic only: times main-thread calls and records the slowest distinct
// paths. No behaviour change.
typedef HANDLE (WINAPI *PFN_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *PFN_CloseHandle)(HANDLE);
static PFN_CreateFileA g_realCreateFileA = NULL;
static PFN_CloseHandle g_realCloseHandle = NULL;
static volatile LONG g_openCount=0, g_openSumUsec=0, g_openMaxUsec=0;
static volatile LONG g_closeCount=0, g_closeSumUsec=0, g_closeMaxUsec=0;

// Slowest distinct opens seen, for identifying WHAT is being opened.
#define SLOW_OPEN_SLOTS 10
#define SLOW_OPEN_MIN_USEC 300
static struct { LONG usec; char path[160]; } g_slowOpens[SLOW_OPEN_SLOTS];
static LONG g_slowOpenCount = 0;
static CRITICAL_SECTION g_slowOpenLock;
static volatile LONG g_slowOpenLockState = 0;

static void RecordSlowOpen(const char *path, LONG usec)
{
    if (!path || usec < SLOW_OPEN_MIN_USEC) return;
    if (InterlockedCompareExchange(&g_slowOpenLockState, 1, 0) == 0) {
        InitializeCriticalSection(&g_slowOpenLock);
        g_slowOpenLockState = 2;
    } else {
        while (g_slowOpenLockState != 2) Sleep(0);
    }
    EnterCriticalSection(&g_slowOpenLock);
    // Keep only the worst offenders: if full, replace the fastest entry, so
    // the table converges on the genuinely expensive opens rather than
    // whichever happened first.
    LONG target = -1;
    if (g_slowOpenCount < SLOW_OPEN_SLOTS) {
        target = g_slowOpenCount++;
    } else {
        LONG minIdx = 0;
        for (LONG i = 1; i < SLOW_OPEN_SLOTS; i++)
            if (g_slowOpens[i].usec < g_slowOpens[minIdx].usec) minIdx = i;
        if (usec > g_slowOpens[minIdx].usec) target = minIdx;
    }
    if (target >= 0) {
        g_slowOpens[target].usec = usec;
        strncpy(g_slowOpens[target].path, path, sizeof(g_slowOpens[0].path) - 1);
        g_slowOpens[target].path[sizeof(g_slowOpens[0].path) - 1] = 0;
    }
    LeaveCriticalSection(&g_slowOpenLock);
}

static HANDLE WINAPI HookedCreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                       LPSECURITY_ATTRIBUTES lpSec, DWORD dwCreationDisposition,
                                       DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    int isMain = ((LONG)GetCurrentThreadId() == g_mainThreadId);
    unsigned __int64 t0 = isMain ? __rdtsc() : 0;
    HANDLE h = g_realCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSec,
                                 dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    if (isMain && g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_openCount);
        InterlockedExchangeAdd(&g_openSumUsec, us);
        if (us > g_openMaxUsec) g_openMaxUsec = us;
        RecordSlowOpen(lpFileName, us);
    }
    return h;
}

static BOOL WINAPI HookedCloseHandle(HANDLE hObject)
{
    int isMain = ((LONG)GetCurrentThreadId() == g_mainThreadId);
    unsigned __int64 t0 = isMain ? __rdtsc() : 0;
    BOOL ok = g_realCloseHandle(hObject);
    if (isMain && g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_closeCount);
        InterlockedExchangeAdd(&g_closeSumUsec, us);
        if (us > g_closeMaxUsec) g_closeMaxUsec = us;
    }
    return ok;
}

static void LogD3DWindow(void)
{
    ApplyFramerateUnlock();
    char line[256];
    LONG nD3d = g_d3dSlotCount;
    for (LONG i = 0; i < nD3d; i++) {
        D3DSlot *sl = &g_d3dSlots[i];
        LONG c = sl->count, sum = sl->sumUsec;
        LONG wc = c - g_prevD3dCount[i], ws = sum - g_prevD3dSum[i];
        g_prevD3dCount[i] = c;
        g_prevD3dSum[i] = sum;
        if (wc <= 0) continue;
        LONG mx = InterlockedExchange(&sl->maxUsec, 0);
        // Only report methods that actually cost something this window -
        // a microsecond-level call rate is noise here.
        if (ws < 1000 && mx < 2000) continue;
        sprintf(line, "[d3d9] %-28s calls=%-6ld total_usec=%-8ld max_usec=%ld",
                sl->name, wc, ws, mx);
        LogLine(line);
    }

    LONG discardTotal = g_lockRectDiscardInjected;
    LONG discardWindow = discardTotal - g_prevLockRectDiscardInjected;
    g_prevLockRectDiscardInjected = discardTotal;
    // Report unconditionally once anything eligible has been seen, including
    // windows where nothing was injected. A silent window used to be
    // ambiguous between "no dynamic textures locked" and "the fix is dead";
    // eligible/firstseen/tracked tell those apart at a glance.
    LONG elig = g_lockRectEligible, firstSeen = g_lockRectFirstSeen;
    if (elig > 0) {
        sprintf(line, "[d3d9] DISCARD inject=%ld this window (total=%ld) | eligible=%ld firstseen=%ld mipskip=%ld tracked=%ld/%d clears=%ld",
                discardWindow, discardTotal, elig, firstSeen, g_lockRectMipSkipped,
                g_seenTextureCount, SEEN_TEX_SLOTS, g_seenTexClears);
        LogLine(line);
    }
    {
        // Present timing on the real device. Reported as its own line rather
        // than folded into the D3DSlot table because it is hooked directly,
        // not through the slot machinery.
        static LONG prevPresentCount = 0, prevPresentSum = 0;
        LONG pc = g_presentCount, ps = g_presentSumUsec;
        LONG dc = pc - prevPresentCount, ds = ps - prevPresentSum;
        prevPresentCount = pc; prevPresentSum = ps;
        if (dc > 0) {
            LONG pmax = InterlockedExchange(&g_presentMaxUsec, 0);
            sprintf(line, "[d3d9] Present: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)",
                    dc, ds / dc, pmax, pc);
            LogLine(line);
        }

        // Real-device UpdateSurface/UpdateTexture/CreateTexture - see the
        // comment above HookedUpdateSurface for why these are being measured
        // for the first time in this run.
        {
            static LONG prevUS=0, prevUSsum=0, prevUT=0, prevUTsum=0, prevCT=0, prevCTsum=0;
            LONG usC=g_updSurfCount, usS=g_updSurfSumUsec, dUS=usC-prevUS, dUSs=usS-prevUSsum;
            prevUS=usC; prevUSsum=usS;
            if (dUS > 0) {
                LONG mx = InterlockedExchange(&g_updSurfMaxUsec, 0);
                sprintf(line, "[d3d9] UpdateSurface: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)",
                        dUS, dUSs/dUS, mx, usC);
                LogLine(line);
            }
            LONG utC=g_updTexCount, utS=g_updTexSumUsec, dUT=utC-prevUT, dUTs=utS-prevUTsum;
            prevUT=utC; prevUTsum=utS;
            if (dUT > 0) {
                LONG mx = InterlockedExchange(&g_updTexMaxUsec, 0);
                sprintf(line, "[d3d9] UpdateTexture: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)",
                        dUT, dUTs/dUT, mx, utC);
                LogLine(line);
            }
            static LONG prevVB=0, prevVBs=0, prevIB=0, prevIBs=0;
            LONG vbC=g_cvbCount, vbS=g_cvbSumUsec, dVB=vbC-prevVB, dVBs=vbS-prevVBs;
            prevVB=vbC; prevVBs=vbS;
            if (dVB > 0) {
                LONG mx = InterlockedExchange(&g_cvbMaxUsec, 0);
                sprintf(line, "[d3d9] CreateVertexBuffer: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)", dVB, dVBs/dVB, mx, vbC);
                LogLine(line);
            }
            LONG ibC=g_cibCount, ibS=g_cibSumUsec, dIB=ibC-prevIB, dIBs=ibS-prevIBs;
            prevIB=ibC; prevIBs=ibS;
            if (dIB > 0) {
                LONG mx = InterlockedExchange(&g_cibMaxUsec, 0);
                sprintf(line, "[d3d9] CreateIndexBuffer: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)", dIB, dIBs/dIB, mx, ibC);
                LogLine(line);
            }
            LONG ctC=g_createTexCount, ctS=g_createTexSumUsec, dCT=ctC-prevCT, dCTs=ctS-prevCTsum;
            prevCT=ctC; prevCTsum=ctS;
            if (dCT > 0) {
                LONG mx = InterlockedExchange(&g_createTexMaxUsec, 0);
                sprintf(line, "[d3d9] CreateTexture: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)",
                        dCT, dCTs/dCT, mx, ctC);
                LogLine(line);
            }
            // Shader creation. Reported per window AND cumulatively so the
            // load-time-vs-gameplay split is visible: a big cumulative total
            // with near-zero per-window counts means it all happens at load
            // and pre-warming would gain nothing.
            static LONG prevVS=0, prevVSsum=0, prevPS=0, prevPSsum=0;
            LONG vsC=g_vsCount, vsS=g_vsSumUsec, dVS=vsC-prevVS, dVSs=vsS-prevVSsum;
            prevVS=vsC; prevVSsum=vsS;
            if (dVS > 0) {
                LONG mx = InterlockedExchange(&g_vsMaxUsec, 0);
                sprintf(line, "[d3d9] CreateVertexShader: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld mainthread=%ld)",
                        dVS, dVSs/dVS, mx, vsC, g_vsMainCount);
                LogLine(line);
            }
            LONG psC=g_psCount, psS=g_psSumUsec, dPS=psC-prevPS, dPSs=psS-prevPSsum;
            prevPS=psC; prevPSsum=psS;
            if (dPS > 0) {
                LONG mx = InterlockedExchange(&g_psMaxUsec, 0);
                sprintf(line, "[d3d9] CreatePixelShader: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld mainthread=%ld)",
                        dPS, dPSs/dPS, mx, psC, g_psMainCount);
                LogLine(line);
            }

            static LONG prevSL=0, prevSLsum=0;
            LONG slC=g_surfLockCount, slS=g_surfLockSumUsec, dSL=slC-prevSL, dSLs=slS-prevSLsum;
            prevSL=slC; prevSLsum=slS;
            static LONG prevCube=0, prevCubeSum=0;
            LONG cbC=g_cubeLockCount, cbS=g_cubeLockSumUsec, dCB=cbC-prevCube, dCBs=cbS-prevCubeSum;
            prevCube=cbC; prevCubeSum=cbS;
            if (dCB > 0) {
                LONG mx = InterlockedExchange(&g_cubeLockMaxUsec, 0);
                sprintf(line, "[d3d9] CubeTexture::LockRect: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)",
                        dCB, dCBs/dCB, mx, cbC);
                LogLine(line);
            }
#if ENABLE_DEFER_UPLOADS
            if (g_deferQueued || g_deferIssued) {
                sprintf(line, "[d3d9] deferred uploads: queued=%ld issued=%ld overflow=%ld depth=%ld/%d (limit %ld/frame)",
                        g_deferQueued, g_deferIssued, g_deferOverflow,
                        g_uploadQueueDepth, MAX_PENDING_UPLOADS, g_deferPerFrame);
                LogLine(line);
            }
#endif
            {
                // The deferred-vs-forward verdict. The original version keyed
                // this off g_maxRtIndexSeen alone and printed "MRT / G-buffer
                // in use (deferred)" - but that max counts
                // SetRenderTarget(n, NULL), i.e. slots being CLEARED. Only a
                // non-NULL bind above slot 0 is real MRT.
                static LONG lastReal = -1, lastAny = -1;
                if (g_maxRealRtIndex != lastReal || g_maxRtIndexSeen != lastAny) {
                    lastReal = g_maxRealRtIndex;
                    lastAny = g_maxRtIndexSeen;
                    sprintf(line, "[rt] slot>0: real_binds=%ld null_unbinds=%ld | "
                                  "max index any=%ld REAL=%ld -> %s",
                            g_mrtRealBinds, g_mrtNullUnbinds,
                            g_maxRtIndexSeen, g_maxRealRtIndex,
                            g_mrtRealBinds == 0
                                ? "SINGLE RT ONLY (forward - MSAA worth investigating)"
                                : "genuine MRT / G-buffer (deferred - MSAA problematic)");
                    LogLine(line);
                }
            }
#if ENABLE_CASCADE_HUNT
            if (g_cascadeRewrites || g_propagations) {
                sprintf(line, "[cascade] near x%ld : render-side rewrites=%ld  sampling-side propagations=%ld",
                        g_nearCascadePct, g_cascadeRewrites, g_propagations);
                LogLine(line);
            }
#endif
            if (g_splitSrcWrites || g_splitSrcSeenNear > 0.0f) {
                sprintf(line, "[split-src] engine near=%.2f far=%.2f (product) | writes=%ld near%%=%ld far%%=%ld",
                        g_splitSrcSeenNear, g_splitSrcSeenFar, g_splitSrcWrites,
                        g_shadowSplitNearPct, g_shadowSplitFarPct);
                LogLine(line);
            }
            if (g_shadowMapRes > 0) {
                sprintf(line, "[shadow] engine res value: want=%ld last_seen=%ld writes=%ld",
                        g_shadowMapRes, g_shadowResLastSeen, g_shadowResWrites);
                LogLine(line);
            }
#if ENABLE_SCALING_MODE
            // Always reported, even when not overriding: the game's OWN
            // scaling default is the thing worth knowing here.
            {
                const char *cur = "?";
                if (g_scalingSeen20 == 3 && g_scalingSeen40 == 0) cur = "None";
                else if (g_scalingSeen20 == 1 && g_scalingSeen40 == 0) cur = "Standard";
                else if (g_scalingSeen20 == 1 && g_scalingSeen40 == 1) cur = "Advanced";
                sprintf(line, "[scaling] engine is %s (+0x20=%ld +0x40=%ld) | want=%ld writes=%ld",
                        cur, g_scalingSeen20, g_scalingSeen40, g_scalingMode, g_scalingWrites);
                LogLine(line);
            }
#endif  // ENABLE_SCALING_MODE
            // Always reported when set, so "is it actually on?" never has to be
            // guessed at again - the last attempt was judged by eye while the
            // toggle was off the whole time.
            // Draw attribution is reported whenever draws exist, NOT only when
            // MSAA is on. The first version gated it behind the MSAA counters,
            // so setting MsaaSamples=0 to survey the UNMODIFIED renderer also
            // switched off the survey - the measurement disabled by the very
            // control it was meant to be independent of.
            if (g_drawsTotal) {
                char dl[320];
                int dopos = sprintf(dl, "[draws] by pass:");
                for (int pi = 0; pi < PASS_COUNT; pi++)
                    dopos += sprintf(dl + dopos, " %s=%ld",
                                     g_passNames[pi], g_drawsByPass[pi]);
                LogLine(dl);
                sprintf(dl, "[draws] by target: sceneColour=%ld linearDepth=%ld "
                            "shadowMaps=%ld other=%ld  (total=%ld) | distinct full-screen "
                            "A8R8G8B8 targets=%ld",
                        g_drawsRtFmt21, g_drawsRtFmt114, g_drawsRtShadow,
                        g_drawsRtOther, g_drawsTotal, g_sceneRtCount);
                LogLine(dl);
                {
                    LONG n = g_sceneRtCount, i;
                    if (n > SCENE_RT_MAX) n = SCENE_RT_MAX;
                    int so2 = sprintf(dl, "[scenert] per-surface draws (total/inMULTI_SAMPLE):");
                    for (i = 0; i < n; i++)
                        so2 += sprintf(dl + so2, "  #%ld=%ld/%ld",
                                       i + 1, g_sceneRtDraws[i], g_sceneRtDrawsMs[i]);
                    LogLine(dl);
                }
            }
            if (g_msaaSamples >= 2 || g_msSubstitutions) {
                // substitutions and resolves should track 1:1 per frame; a gap
                // means the pass ended without the resolve boundary firing and
                // the engine is sampling an unresolved (stale) texture.
                // getLies counts engine captures of our surfaces through the
                // Get methods (theory 1 - nonzero means it DOES push/pop
                // bindings); dsOutside must stay 0 now the lies seal the leak;
                // failedALL covers failures in passes where failedMS is blind;
                // rsWrites/forced say whether the engine touches the two
                // multisample render states at all (theories 2/3).
                char ml[384];
                sprintf(ml, "[msaa] x%ld subs=%ld resolves=%ld resolveFail=%ld %ux%u | "
                            "draws total=%ld whileMS=%ld failedMS=%ld failedALL=%ld | "
                            "getLies rt=%ld ds=%ld dsOutside=%ld | rsWrites=%ld forced=%ld | "
                            "clearZ=%.3f | texMemFreeMB=%ld | zfuncSeen=0x%lX zfuncForced=%ld stencilNE=%ld | "
                            "r32f subs=%ld resolves=%ld fails=%ld | a2c=%ld | "
                            "grab sync=%ld foreignW=%ld suppressed=%ld",
                        g_msaaSamples, g_msSubstitutions, g_msResolves, g_msFailures,
                        g_msW, g_msH, g_drawsTotal, g_drawsWhileMs, g_drawsFailedMs,
                        g_drawsFailedAll, g_msGetRtLies, g_msGetDsLies,
                        g_msDsOutsideRebind, g_rsMsWrites, g_rsMsForced, g_lastClearZ,
                        g_availTexMemMB, (unsigned long)g_zfuncSeenMask, g_zfuncForced,
                        g_stencilNonAlways, g_msR32fSubs, g_msR32fResolves, g_msR32fFails,
                        g_a2cMirrored, g_msSyncResolves, g_msForeignWrites,
                        g_msSuppressedSubs);
                LogLine(ml);
            }
            // SSAA status, on its OWN condition - reported whenever a scale is
            // set even if nothing else is active, because "the option is on and
            // nothing happened" is precisely the case that needs to be
            // distinguishable from "the option is off". The three numbers
            // answer three different failure modes:
            //   internal size  - did the engine ACCEPT a size above the
            //                    backbuffer, or clamp/ignore it
            //   linearBlits    - is the downsample actually being filtered
            //                    (0 while active means POINT decimation, i.e.
            //                    full cost and no antialiasing whatsoever)
            //   linearFails    - the driver refused LINEAR on StretchRect and
            //                    the engine's own filter was used instead
            if (g_ssaaScale != 100 || g_ssaaWrites) {
                unsigned int iw = 0, ih = 0;
                GetInternalRenderSize(&iw, &ih);
                char sl[256];
                sprintf(sl, "[ssaa] scale=%ld%% mode=%s effective=%ux%u backbuf=%ux%u | "
                            "descScaled=%ld rebuilds=%ld/blocked=%ld writes=%ld "
                            "linearBlits=%ld linearFails=%ld outRes=%ux%u/%ld/fail=%ld%s",
                        g_ssaaScale,
                        g_ssaaMode == 1 ? "descriptor" : "resolution",
                        iw, ih, g_backbufW, g_backbufH,
                        g_ssaaDescScaled, g_ssaaRebuildsAllowed, g_ssaaRebuildsBlocked,
                        g_ssaaWrites,
                        g_ssaaLinearBlits, g_ssaaLinearFails,
                        g_ssaaInterW, g_ssaaInterH, g_ssaaInterBlits, g_ssaaInterFails,
                        // The one-line verdict, and it is deliberately derived
                        // from what is TRUE at runtime rather than from which
                        // mode was requested: supersampling happens only when
                        // the rendered size exceeds the presented size.
                        // `effective` comes from GetInternalRenderSize, which
                        // both modes keep honest, so this reads correctly for
                        // either.
                        (g_ssaaScale != 100 && g_backbufW && iw > g_backbufW)
                            ? "  SUPERSAMPLING" : "  (no downsample - not supersampling)");
                LogLine(sl);
            }
            // subs counting while a pick is active proves the hash identified
            // a shader the engine actually binds; subs staying 0 with objs>0
            // means that candidate is not bound right now (e.g. a menu);
            // objs=0 means none of the candidate shaders were created.
            // variants>0 proves the pattern matcher recognised the cutout
            // shaders; binds>0 proves they are actually being drawn with.
            // buildFails>0 means the synthesised bytecode was rejected by the
            // runtime, i.e. the transform itself is wrong.
            // Boot survived; back to buffered logging - UNLESS LogFlush=1.
            // Buffering is what makes a crash log stop at an arbitrary point:
            // the last lines sit unflushed and die with the process, so the
            // log's ending looks like the crash site and is not. That misread
            // cost real time chasing the intermittent startup failure.
            // LogFlush=1 keeps every line on disk at the documented cost
            // below, which is the right trade while reproducing a crash.
            if (!g_logFlushAlways) g_bootFlush = 0;
            // Reported on their OWN flags. These were nested inside the A2C
            // condition, so with A2C off and no variants yet they printed
            // nothing at all - which is indistinguishable from the feature
            // running and doing nothing, and cost a wasted test run.
#if ENABLE_CUTOUT_AA
            if (g_fringeFoliage || g_fringeDraws) {
                sprintf(line, "[fringe] on=%ld passes=%ld buildFails=%ld sharpness=%ld variants=%ld",
                        g_fringeFoliage, g_fringeDraws, g_fringeBuildFails,
                        g_a2cSharpness, g_a2cVariantCount);
                LogLine(line);
            }
            if (g_a2cEnable || g_a2cVariantCount) {
                sprintf(line, "[ssaa] on=%ld multiDraws=%ld buildFails=%ld offsetReg=c%ld samples=%ld",
                        g_ssaaFoliage, g_ssaaDraws, g_ssaaBuildFails,
                        g_ssaaOffsetReg, g_msaaSamples);
                LogLine(line);
                sprintf(line, "[a2c] enable=%ld variants=%ld binds=%ld buildFails=%ld "
                              "noRoom=%ld blockedByBlend=%ld sharpness=%ld (MsaaSamples=%ld)",
                        g_a2cEnable, g_a2cVariantCount, g_a2cBinds, g_a2cBuildFails,
                        g_a2cSkipNoRoom, g_a2cBlockedBlend,
                        g_a2cSharpness, g_msaaSamples);
                LogLine(line);
            }
            // Candidate list for the identify walk: only cutout shaders that
            // are ACTUALLY DRAWN in the scene pass with blending off, ranked
            // by draw count. Grass and barriers are drawn constantly, so they
            // sit near the top - which turns 1344 texkill shaders into a
            // handful worth stepping through by hand.
            for (LONG r = 0; r < g_psRankCount && r < 14; r++) {
                LONG idx = g_psRankIdx[r];
                if (idx < 0 || idx >= PS_MAP_MAX) continue;
                sprintf(line, "[shader] #%ld ps_%08X recent=%ld taps=%u %s%s",
                        r + 1, g_psMap[idx].hash, g_psMap[idx].recent,
                        g_psMap[idx].taps,
                        g_psMap[idx].hasVariant ? "CUTOUT" : "-",
                        (g_psIdentify == r + 1) ? "   <== MAGENTA" : "");
                LogLine(line);
            }
            if (g_fxaaPick || g_fxaaSubs) {
                sprintf(line, "[fxaa] pick=%ld objs=%ld subs=%ld passthrough=%s tint=%s",
                        g_fxaaPick, g_psKillObjCount, g_fxaaSubs,
                        g_fxaaPassthrough ? "ready" : "none",
                        g_psTintObj ? "ready" : "none");
                LogLine(line);
            }
            // Post-chain shader table (discovery instrument, DumpShaders only):
            // every pixel shader that drew onto the scene-sized colour target
            // during the post window, with identity hash + tap count from the
            // creation-time map. minPrim <= 2 marks fullscreen-quad passes -
            // the post-AA MUST be one of those lines.
            if (g_dumpShaders && g_postPsSeenCount) {
                // Truncation must announce itself - see the PS_HASH_MAX note.
                if (g_psMapCount >= PS_MAP_MAX || g_psHashCount >= PS_HASH_MAX)
                    LogLine("[postps] *** CAP HIT: shader map/dump truncated, census NOT complete ***");
                LONG n = g_postPsSeenCount;
                if (n > POST_PS_MAX) n = POST_PS_MAX;
                for (LONG i = 0; i < n; i++) {
                    void *obj = g_postPsSeen[i].obj;
                    DWORD hsh = 0; UINT taps = 0;
                    LONG m = g_psMapCount;
                    if (m > PS_MAP_MAX) m = PS_MAP_MAX;
                    for (LONG j = 0; j < m; j++)
                        if (g_psMap[j].obj == obj) { hsh = g_psMap[j].hash; taps = g_psMap[j].taps; break; }
                    // Pass names as a compact list, and the target format:
                    // fmt 21 = A8R8G8B8 (scene), 22 = X8R8G8B8 (BACKBUFFER -
                    // where a final post-AA writes, and what the old gate
                    // wrongly excluded).
                    char pl[128]; int po = 0; pl[0] = 0;
                    for (int pi = 0; pi < PASS_COUNT; pi++)
                        if (g_postPsSeen[i].passMask & (1L << pi))
                            po += sprintf(pl + po, "%s%s", po ? "," : "", g_passNames[pi]);
                    // taps here is the CREATION-time count from the corrected
                    // walker; >=5 on a quad pass would be the filter shape
                    // the offline census says does not exist at full res.
                    sprintf(line, "[postps] #%ld ps_%08X draws=%ld taps=%u rt=%ldx%ld fmt=%ld%s%s [%s]",
                            i + 1, hsh, g_postPsSeen[i].draws, taps,
                            g_postPsSeen[i].w, g_postPsSeen[i].h, g_postPsSeen[i].fmt,
                            (g_postPsSeen[i].fmt == 22) ? "(BACKBUFFER)" : "",
                            (taps >= 5) ? "  <== MULTI-TAP QUAD (filter shape)" : "", pl);
                    LogLine(line);
                }
            }
#endif  // ENABLE_CUTOUT_AA / shader-diag reporting
            if (g_shadowBufResPct > 0) {
                // viewports_scaled is the number that matters: textures and
                // viewports must move together or the pass renders into the
                // wrong area. If textures=N and viewports=0, that is the exact
                // failure already seen (dark world / blocky shifted shadows).
                sprintf(line, "[shadowbuf] pct=%ld descriptors_scaled=%ld (stock %ux%u)",
                        g_shadowBufResPct, g_sbufCtorScaled,
                        g_backbufW / 2, g_backbufH / 2);
                LogLine(line);
            }
            // zero_deltas is the number that matters: it counts frames where
            // the engine's own delta came out as literally 0, i.e. the failure
            // this fix exists for. If it stays 0 while the fix is on, the game
            // is not actually running above 59.94 and the A/B proves nothing.
            // The limiter spins instead of sleeping whenever the frame's
            // remaining time is below this mark, so compare it against the
            // frame interval: 16.68ms at 59.94, 10.00ms at 100fps. A mark at
            // or above the interval means EVERY frame is a busy-spin.
            {
                LONG fpsX100 = g_targetFpsX100;
                LONG intervalUs = (fpsX100 > 0) ? (LONG)(100000000LL / fpsX100) : 0;
                sprintf(line, "[limiter] sleep granularity high-water = %ld us | "
                              "interval %ld us | guard=%ld us writes=%ld%s",
                        g_lastGranUs, intervalUs, g_spinGuardUs, g_spinGuardWrites,
                        (intervalUs > 0 && g_lastGranUs >= intervalUs)
                            ? "  *** >= INTERVAL: limiter busy-spins every frame ***" : "");
                LogLine(line);
            }
            // Simultaneous-target count per pass. The old "max SetRenderTarget
            // index = 3" was one global number and could not say WHICH stage
            // binds an MRT set - which is the actual forward-vs-deferred
            // evidence.
            if (g_logPassRts) {
                char pl[256];
                int po = sprintf(pl, "[pass] max simultaneous RT index per pass:");
                for (int pi = 0; pi < PASS_COUNT; pi++)
                    po += sprintf(pl + po, " %s=%ld", g_passNames[pi], g_passMaxRtIndex[pi]);
                LogLine(pl);
            }
            if (g_gpuSyncSkip || g_gpuFenceSkipped) {
                sprintf(line, "[gpufence] skip=%ld fences_skipped=%ld | FUN_00ac3040 avg=%.0fus max=%ldus",
                        g_gpuSyncSkip, g_gpuFenceSkipped,
                        g_funcs[FN_AC3040].durationCount
                            ? (double)g_funcs[FN_AC3040].durationSumCycles
                              / (double)g_funcs[FN_AC3040].durationCount
                              / (g_cyclesPerUsec > 0.0 ? g_cyclesPerUsec : 1.0)
                            : 0.0,
                        g_funcs[FN_AC3040].maxUsec);
                LogLine(line);
            }
            if (g_simDeltaFix || g_sdReplaced) {
                sprintf(line, "[simdelta] fix=%ld replaced=%ld zero_deltas_fixed=%ld last %ld -> %ld (5005 = one 59.94 frame)",
                        g_simDeltaFix, g_sdReplaced, g_sdZeroFixed,
                        g_sdLastOrig, g_sdLastNew);
                LogLine(line);
            }
#if ENABLE_TALK_TIMER
            if (g_talkTimerPct > 0 || g_talkStepPatched) {
                sprintf(line, "[talk] timer rate=%ld%% patched=%d step=%.4f/frame "
                              "(stock 0.05) orig_operand=0x%08X",
                        g_talkTimerPct, g_talkStepPatched, g_talkStep,
                        g_talkStepOrigDisp);
                LogLine(line);
            }
#endif
#if ENABLE_SHADOW_SCALE
            if (g_shadowScaled || g_shadowScaleFail) {
                sprintf(line, "[shadow] scaling: applied=%ld failed_fellback=%ld (x%ld) | tracked_surfaces=%ld viewports_scaled=%ld",
                        g_shadowScaled, g_shadowScaleFail, g_shadowScale,
                        g_shadowSurfaceCount, g_viewportScaled);
                LogLine(line);
            }
#endif
#if ENABLE_MANAGED_POOL
            if (g_managedRewriteCount || g_managedRewriteFail) {
                sprintf(line, "[d3d9] MANAGED pool rewrite: applied=%ld failed_reverted=%ld",
                        g_managedRewriteCount, g_managedRewriteFail);
                LogLine(line);
            }
#endif
            {
                LONG pDefault = g_texPoolRequested[0], pManaged = g_texPoolRequested[1];
                LONG pSysmem = g_texPoolRequested[2], pScratch = g_texPoolRequested[3];
                if (pDefault || pManaged || pSysmem || pScratch) {
                    sprintf(line, "[d3d9] CreateTexture pool histogram (cumulative): DEFAULT=%ld MANAGED=%ld SYSTEMMEM=%ld SCRATCH=%ld",
                            pDefault, pManaged, pSysmem, pScratch);
                    LogLine(line);
                }
            }
#if ENABLE_TEXTURE_POOL
            if (g_texPoolHits || g_texPoolMisses) {
                sprintf(line, "[texpool] hits=%ld misses=%ld returned=%ld evicted=%ld pooled=%ld/%d",
                        g_texPoolHits, g_texPoolMisses, g_texPoolReturned, g_texPoolEvicted,
                        g_texPoolCount, TEXPOOL_MAX_ENTRIES);
                LogLine(line);
            }
#endif
            if (g_cubeStagingHits || g_cubeStagingFallback) {
                sprintf(line, "[d3d9] cube staging: hits=%ld fallback=%ld updatefail=%ld inflight=%ld",
                        g_cubeStagingHits, g_cubeStagingFallback, g_cubeStagingUpdateFail,
                        g_inflightCubeCount);
                LogLine(line);
            }
            if (dSL > 0) {
                LONG mx = InterlockedExchange(&g_surfLockMaxUsec, 0);
                sprintf(line, "[d3d9] Surface::LockRect: calls=%ld avg_usec=%ld max_usec=%ld (total=%ld)",
                        dSL, dSLs/dSL, mx, slC);
                LogLine(line);
            }
            if (g_surfStagingHits || g_surfStagingFallback) {
                sprintf(line, "[d3d9] surface staging: hits=%ld fallback=%ld updatefail=%ld inflight=%ld",
                        g_surfStagingHits, g_surfStagingFallback, g_surfStagingUpdateFail,
                        g_inflightSurfCount);
                LogLine(line);
            }
        }

        {
            static LONG prevOpen=0, prevOpenSum=0, prevClose=0, prevCloseSum=0;
            LONG oc=g_openCount, os=g_openSumUsec, dO=oc-prevOpen, dOs=os-prevOpenSum;
            prevOpen=oc; prevOpenSum=os;
            if (dO > 0) {
                LONG mx = InterlockedExchange(&g_openMaxUsec, 0);
                sprintf(line, "[probe] main-thread CreateFileA: calls=%ld total_usec=%ld avg=%ld max=%ld (cum=%ld)",
                        dO, dOs, dOs/dO, mx, oc);
                LogLine(line);
            }
            LONG cc=g_closeCount, cs=g_closeSumUsec, dC=cc-prevClose, dCs=cs-prevCloseSum;
            prevClose=cc; prevCloseSum=cs;
            if (dC > 0) {
                LONG mx = InterlockedExchange(&g_closeMaxUsec, 0);
                sprintf(line, "[probe] main-thread CloseHandle: calls=%ld total_usec=%ld avg=%ld max=%ld (cum=%ld)",
                        dC, dCs, dCs/dC, mx, cc);
                LogLine(line);
            }
            // Dump the worst offenders periodically - this is what identifies
            // WHICH files are expensive, which is the whole point.
            static LONG dumpTick = 0;
            if (g_slowOpenLockState == 2 && ++dumpTick % 20 == 0) {
                EnterCriticalSection(&g_slowOpenLock);
                for (LONG i = 0; i < g_slowOpenCount; i++) {
                    sprintf(line, "[probe]   slow open %ldus: %s", g_slowOpens[i].usec, g_slowOpens[i].path);
                    LogLine(line);
                }
                LeaveCriticalSection(&g_slowOpenLock);
            }
        }

        static LONG prevSleepCount = 0, prevSleepUsec = 0;
        LONG sc2 = g_mainSleepCount, su = g_mainSleepUsec;
        LONG dsc = sc2 - prevSleepCount, dsu = su - prevSleepUsec;
        prevSleepCount = sc2; prevSleepUsec = su;
        if (dsc > 0) {
            LONG smax = InterlockedExchange(&g_mainSleepMaxUsec, 0);
            sprintf(line, "[probe] main-thread Sleep: calls=%ld total_usec=%ld avg_usec=%ld max_usec=%ld",
                    dsc, dsu, dsu / dsc, smax);
            LogLine(line);

            // Ranked call sites (sampled 1:256), converted to Ghidra VAs the
            // same way every other address in this file is
            // (runtime - module base + 0x00400000). The first version logged
            // RAW runtime addresses with a comment claiming they mapped
            // directly onto the listing - they do not, the module is based at
            // 0x00100000 here, and the resulting 0x300000 error pointed at a
            // small destructor that never calls Sleep at all.
            LONG used = g_sleepCallerUsed;
            for (LONG rank = 0; rank < 4; rank++) {
                LONG best = -1, bestCount = 0;
                for (LONG i = 0; i < used; i++) {
                    if (g_sleepCallers[i].count > bestCount) {
                        int already = 0;
                        for (LONG r = 0; r < rank; r++) {
                            // skip ones already printed this window
                            if (g_sleepCallers[i].addr == g_sleepTopPrinted[r]) { already = 1; break; }
                        }
                        if (!already) { best = i; bestCount = g_sleepCallers[i].count; }
                    }
                }
                if (best < 0) break;
                g_sleepTopPrinted[rank] = g_sleepCallers[best].addr;
                sprintf(line, "[probe]   sleep caller #%ld: ghidra=%08lX samples=%ld",
                        rank + 1,
                        (unsigned long)(g_sleepCallers[best].addr - g_mainModBase + 0x00400000),
                        bestCount);
                LogLine(line);
            }
        }
    }

    if (g_stagingHits || g_stagingFallback) {
        sprintf(line, "[d3d9] staging: hits=%ld fallback=%ld updatefail=%ld pool=%ld/%d recycled=%ld inflight=%ld",
                g_stagingHits, g_stagingFallback, g_stagingUpdateFail,
                g_stagingCount, MAX_STAGING, g_stagingRecycled, g_inflightCount);
        LogLine(line);
    }

}

// v12: the export-hook approach (v11) was proven wrong by its own integrity
// check - something (RTSS and/or Steam Overlay, both confirmed present in
// the captured D3D9 stacks) re-patched Direct3DCreate9's export with a JMP
// into its own VirtualAlloc'd trampoline (target address didn't resolve to
// any loaded module), completely replacing our JMP rather than chaining
// through it. Waiting to install "after" it is not reliable - no fixed delay
// can be trusted, and the overlay could equally well install first and get
// overwritten in turn by us or a third party.
//
// The fix is to stop needing to intercept the game's OWN CreateDevice call
// at all. COM vtables are shared per implementation CLASS, not per instance
// - IDirect3DDevice9's Present/CreateTexture/Lock/etc. slots point at the
// same driver code for every HAL device on the same adapter, regardless of
// which caller created it. So instead of racing to catch the game's device,
// this creates a small throwaway HAL device of our own (invisible, windowed,
// on a background thread, never in DllMain - see below for why), reads ITS
// vtable, and patches the slots there. That patches the exact same memory
// the game's real device will use, with no dependency on being first, last,
// or even present in whatever chain of hooks a third-party overlay builds.
// This is the same technique tools like ReShade use for this exact reason.
//
// Must run from a background thread, not DllMain: this ACTUALLY CALLS
// Direct3DCreate9/CreateDevice (unlike the abandoned export-patch approach,
// which only touched memory) and the reference mod's own comment about
// d3d9.dll loading additional DLLs on first init - a real deadlock risk
// against the loader lock DllMain holds - applies directly here.
// Which d3d9.dll actually backs the process: Microsoft's, or a wrapper
// (DXVK, ReShade, a HelixMod shim...) dropped in the game folder. The test is
// the module's own path, not its name - every wrapper is called d3d9.dll by
// necessity, and only the real one lives in System32. The DXVK sub-check is
// informational: the behaviour gates key off "not Microsoft's", because the
// vtable-sharing hazard belongs to any wrapper that implements the whole API
// in one class, not to DXVK specifically.
static void IdentifyD3D9Provider(HMODULE hD3D9)
{
    char modPath[MAX_PATH] = "";
    char sysDir[MAX_PATH] = "";
    if (!GetModuleFileNameA(hD3D9, modPath, MAX_PATH)) {
        LogLine("[d3d9] provider: GetModuleFileName failed - assuming system d3d9.dll");
        return;
    }
    GetSystemDirectoryA(sysDir, MAX_PATH);
    size_t sysLen = strlen(sysDir);
    int inSystem = (sysLen > 0 && _strnicmp(modPath, sysDir, sysLen) == 0);
    if (!inSystem) g_d3d9IsThirdParty = 1;

    // DXVK stamps "DXVK" into its version resource. Deliberately NOT read via
    // GetFileVersionInfo: this module IS version.dll, so those names resolve
    // back into proxy.c (or, worse, produce a self-import) - a needless piece
    // of re-entrancy for a line of log text. The resource is already mapped
    // as part of the module image, so scan that directly, bounded by the PE
    // header's own SizeOfImage.
    if (g_d3d9IsThirdParty) {
        const unsigned char *base = (const unsigned char *)hD3D9;
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                DWORD span = nt->OptionalHeader.SizeOfImage;
                const IMAGE_DATA_DIRECTORY *res =
                    &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
                DWORD from = 0;
                if (res->VirtualAddress && res->Size && res->VirtualAddress + res->Size <= span) {
                    from = res->VirtualAddress;      // narrow to .rsrc when we can
                    span = res->VirtualAddress + res->Size;
                }
                // UTF-16LE "DXVK". Non-executable mapped pages, but the walk is
                // guarded anyway - a malformed third-party PE must not be fatal.
                __try {
                    for (DWORD i = from; i + 8 <= span; i++) {
                        if (base[i] == 'D' && base[i + 1] == 0 &&
                            base[i + 2] == 'X' && base[i + 3] == 0 &&
                            base[i + 4] == 'V' && base[i + 5] == 0 &&
                            base[i + 6] == 'K' && base[i + 7] == 0) {
                            g_d3d9IsDxvk = 1;
                            break;
                        }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    LogLine("[d3d9] provider: image scan faulted, DXVK marker undetermined");
                }
            }
        }
    }

    char line[MAX_PATH + 96];
    sprintf(line, "[d3d9] provider: %s (system=%d thirdparty=%ld dxvk=%ld)",
            modPath, inSystem, g_d3d9IsThirdParty, g_d3d9IsDxvk);
    LogLine(line);

    if (g_d3d9IsDxvk) {
        // Not enforced - just stated, because it is a real behavioural fork
        // and silently rewriting the user's config would hide it. The whole
        // staging/DISCARD family exists to route around AMDXN32's blocking
        // lock path (PROGRESS.md, "This explains DXVK completely"), which
        // does not exist once d3d9 calls terminate in Vulkan instead.
        LogLine("[d3d9] DXVK detected: the staging/DISCARD fixes target the native AMD "
                "D3D9 driver stall, which DXVK removes outright - consider StagingUpload/"
                "StagingSurface/StagingCube/DiscardFix=0 under DXVK");
    }
}

static void ProbeD3D9ForVtable(void)
{
    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (!hD3D9) hD3D9 = LoadLibraryA("d3d9.dll");
    if (!hD3D9) { LogLine("[d3d9] probe: d3d9.dll not found"); return; }

    IdentifyD3D9Provider(hD3D9);

    // Found via an actual WER crash dump (2026-08-07): ACCESS_VIOLATION
    // (0xC0000005) inside vulkan-1.dll, early in mod startup, reproducible
    // under DXVK with mod+DXVK running. The throwaway device below is
    // "harmless" ONLY because Microsoft's d3d9.dll hands the probe a cheap,
    // genuinely separate software-VP device - true for System32's DLL,
    // never verified for a wrapper. Under DXVK there is no such thing: this
    // call creates a SECOND real, independent Vulkan-backed device (own
    // shader-compiler threads, own swapchain) racing the game's own real
    // device through the same DXVK/Vulkan-loader machinery in one process -
    // a configuration DXVK was never designed to see, and evidently doesn't
    // tolerate. ProbeDeviceThunks (the earlier, narrower gate on installing
    // thunks on this device's vtable) does not help here: it only gates what
    // happens AFTER this device already exists, not whether creating it in
    // the first place is safe - which is the part that was actually
    // crashing. Skip creating it outright under any third-party d3d9.dll;
    // HookRealDevicePresent below sources the resource-vtable hooks (the
    // actual staging fix) from the game's own real device instead, so
    // nothing functional is lost.
    // ...and the same applies to a device-WRAPPING mod, which the
    // third-party-d3d9 test above cannot see. The HD GUI mod is a version.dll:
    // d3d9.dll really is System32's (the log says system=1 thirdparty=0), so
    // that gate never fires, yet the mod wraps EVERY device CreateDevice
    // returns - including this throwaway one. That is precisely the
    // "never verified for a wrapper" case the comment above flags.
    //
    // Evidence it is not theoretical (2026-08-13): 7 crashes in 21 launches,
    // every one an access violation inside VERSION.dll, at two different
    // sites. Diffing crashed against working runs showed the difference is
    // WHICH DEVICE IS CREATED FIRST - working runs log this 4x4 probe device,
    // crashed runs have the game's real device ahead of it. A race between our
    // deferred thread and the game's main thread, through a wrapper that is
    // hooking CreateDevice at the same moment.
    //
    // That also explains what nothing else could: why the failure rate moved
    // with UNRELATED I/O changes (per-line log flushing, a config write, the
    // HD mod's texture dumping) - all of them shift thread timing - while
    // HDTexPush, ForceStdD3D9 and the staging redirects made no difference at
    // all, because none of them touch the race.
    //
    // Nothing functional is lost: HookRealDevicePresent sources the
    // resource-vtable hooks from the game's own real device.
    // Re-checked HERE rather than trusting g_hdTexNotify alone: that flag is
    // set by DetectHdTexInterop earlier on this thread, and whether version.dll
    // was loaded at THAT moment is itself timing-dependent - exactly the kind
    // of assumption this whole investigation was built on and burned by.
    // Asking the loader directly, at the point of use, cannot be stale.
    // Detected by WHERE version.dll loaded from, not by an export.
    //
    // The first version of this guard asked for HDTex_InteropVersion, which
    // only the interop-enabled BUILD of the HD GUI mod exports. That build
    // exists on this machine; the PUBLIC one does not export it (verified:
    // the symbol is present in the deployed version.dll and absent from
    // version.dll.bak-20260628, the stock release). So on any normal user's
    // install the guard would not fire, the probe device would be created,
    // and the exact race this whole fix exists to prevent would come back -
    // a release blocker that would have shipped invisibly, because the only
    // machine it was ever tested on is the one that cannot reproduce it.
    //
    // What actually matters is not "is this the HD GUI mod" but "is something
    // proxying a system DLL from the game directory", because that is what
    // wraps the device. A version.dll resolved from anywhere other than the
    // system directory is by definition a proxy - the same test
    // IdentifyD3D9Provider already applies to d3d9.dll. This also covers
    // wrappers we have never heard of, at no cost.
    int wrapperPresent = 0;
    {
        HMODULE hv = GetModuleHandleA("version.dll");
        if (hv) {
            char modPath[MAX_PATH] = "", sysDir[MAX_PATH] = "";
            if (GetModuleFileNameA(hv, modPath, MAX_PATH)) {
                GetSystemDirectoryA(sysDir, MAX_PATH);
                size_t sysLen = strlen(sysDir);
                // Unknown path is treated as a proxy: skipping the probe costs
                // nothing (HookRealDevicePresent covers the same ground), while
                // guessing wrong the other way crashes the game on startup.
                if (sysLen == 0 || _strnicmp(modPath, sysDir, sysLen) != 0)
                    wrapperPresent = 1;
            } else {
                wrapperPresent = 1;
            }
        }
    }
    if (g_d3d9IsThirdParty || g_hdTexNotify || wrapperPresent) {
        LogLine(g_d3d9IsThirdParty
                ? "[d3d9] probe SKIPPED (third-party d3d9.dll) - a second device here "
                  "is what crashed DXVK; resource hooks now come from the game's own "
                  "real device via HookRealDevicePresent"
                : "[d3d9] probe SKIPPED (device-wrapping mod present) - a second device "
                  "racing the game's own through the wrapper is what crashes it; "
                  "resource hooks come from the real device via HookRealDevicePresent");
        return;
    }

    typedef IDirect3D9 *(WINAPI *PFN_Direct3DCreate9)(UINT);
    PFN_Direct3DCreate9 create9 = (PFN_Direct3DCreate9)GetProcAddress(hD3D9, "Direct3DCreate9");
    if (!create9) { LogLine("[d3d9] probe: Direct3DCreate9 export not found"); return; }

    IDirect3D9 *d3d = create9(D3D_SDK_VERSION);
    if (!d3d) { LogLine("[d3d9] probe: Direct3DCreate9 returned NULL"); return; }

    // GetDesktopWindow() as the focus window and Windowed=TRUE means this
    // never touches exclusive-fullscreen mode or the game's own window/device
    // in any way - a fully independent, throwaway object that exists only
    // long enough to read its vtable.
    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 4;
    pp.BackBufferHeight = 4;

    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, GetDesktopWindow(),
                                         D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
                                         &pp, &dev);
    if (FAILED(hr) || !dev) {
        char line[128];
        sprintf(line, "[d3d9] probe: throwaway CreateDevice failed hr=0x%08lX", (unsigned long)hr);
        LogLine(line);
        IDirect3D9_Release(d3d);
        return;
    }

    LogLine("[d3d9] probe: throwaway HAL device created, patching shared vtable");
    HookDeviceVtable(dev);

    // Safe to tear down immediately: the vtable itself lives in the driver's
    // static data, not in this instance, so releasing the object we used to
    // reach it does not undo the patch.
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
}

// ---- Shader identity capture (v19) -----------------------------------------
// v18 confirmed the shader-compile queue costs real time (up to 22ms/call)
// and correlates with roughly half of the worst frames. Open question from
// there: is the same shader recompiling on every reproducible crossing
// (a bounded cache getting evicted between visits), or is a genuinely
// different shader variant/permutation needed each time (feature-combination
// explosion)? Those have different implications and this distinguishes them
// directly - track every compiled shader's identity (the tag argument
// FUN_00a957a0 passes into the renderer's create-shader call) and flag
// whenever the SAME identity compiles more than once.
//
// FUN_00a957a0 (the shader object constructor) has an SEH prologue
// (PUSH -1; PUSH <handler>; MOV EAX,FS:[0], confirmed via disassembly - the
// same pattern that crashed the return-hijack hooks on FN_A01A00/FN_A015B0
// early in this investigation). Return-address hijacking is NOT used here -
// this only needs to READ one incoming argument, not time the call, so the
// detour reads it and jumps straight to a trampoline of the stolen bytes
// without ever touching the return address. The function's own SEH setup
// therefore executes exactly as it would unpatched, just relocated into the
// trampoline - the specific failure mode that broke the two SEH functions
// (hijacking mid-unwind) cannot occur when the return path is never touched.
// v19 result: total=4349 compiles in one session blew straight through a
// 1024-slot table (distinct saturated at exactly 1024), meaning most of the
// session's activity went untracked for repeat-detection after that point -
// all 18 confirmed repeats were also seenCount=2 at last_seen=0.0s (the same
// instant, not "revisited a minute later"), which isn't yet evidence either
// way on the actual question. Bumped generously: even 16384 entries is
// ~196KB, trivial, and a linear scan over that per (rare, multi-millisecond)
// compile call is negligible next to the cost being measured.
#define MAX_SHADER_IDS 16384
typedef struct {
    DWORD id;
    LONG lastSeenUsec;
    LONG seenCount;
    DWORD contentSum; // checksum of bytes AT the id pointer, not the pointer itself
    int contentValid;
} ShaderIdEntry;
static ShaderIdEntry g_shaderIds[MAX_SHADER_IDS];
static CRITICAL_SECTION g_shaderIdLock;

static void *g_trampoline_a957a0_observe = NULL;

// id is used directly as a pointer elsewhere in the real function (passed to
// a "create shader from bytecode" virtual call), so it may be a pointer into
// a reused scratch/pool buffer rather than a stable per-shader identity - two
// DIFFERENT shaders could coincidentally get the same address if one is freed
// and another allocated in its place between calls, which would make a raw
// pointer-value "repeat" a false positive. This checksums the actual bytes
// AT that address (bounded, SEH-guarded read - id is untrusted, could be
// unmapped or not really a pointer in some code path) so a "repeat" can be
// confirmed as the same CONTENT, not just the same numeric value.
static DWORD SafeChecksum(DWORD id, int *validOut)
{
    DWORD sum = 0;
    *validOut = 0;
    __try {
        volatile const unsigned char *p = (volatile const unsigned char *)(UINT_PTR)id;
        for (int i = 0; i < 32; i++) {
            sum = (sum << 1 | sum >> 31) ^ p[i]; // rotate-xor, order-sensitive
        }
        *validOut = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sum = 0;
    }
    return sum;
}

static void __cdecl OnShaderCreate_C(DWORD id)
{
    InterlockedIncrement(&g_shaderCompileTotal);
    LONG now = NowUsec();
    int contentValid = 0;
    DWORD contentSum = SafeChecksum(id, &contentValid);

    EnterCriticalSection(&g_shaderIdLock);
    LONG n = g_shaderIdCount;
    int found = -1;
    for (LONG i = 0; i < n; i++) {
        if (g_shaderIds[i].id == id) { found = i; break; }
    }
    if (found >= 0) {
        LONG agoUsec = now - g_shaderIds[found].lastSeenUsec;
        int prevValid = g_shaderIds[found].contentValid;
        DWORD prevSum = g_shaderIds[found].contentSum;
        g_shaderIds[found].lastSeenUsec = now;
        g_shaderIds[found].seenCount++;
        g_shaderIds[found].contentSum = contentSum;
        g_shaderIds[found].contentValid = contentValid;
        LONG seenCount = g_shaderIds[found].seenCount;
        LeaveCriticalSection(&g_shaderIdLock);
        InterlockedIncrement(&g_shaderCompileRepeats);
        const char *verdict = (!prevValid || !contentValid) ? "UNVERIFIED(unreadable)"
                             : (prevSum == contentSum) ? "CONTENT_MATCH(genuine repeat)"
                             : "CONTENT_DIFFERS(address reuse, NOT a real repeat)";
        char line[224];
        sprintf(line, "[shader] REPEAT compile id=%08lX seenCount=%ld last_seen=%.1fs_ago %s",
                (unsigned long)id, seenCount, (double)agoUsec / 1000000.0, verdict);
        LogLine(line);
    } else {
        if (n < MAX_SHADER_IDS) {
            g_shaderIds[n].id = id;
            g_shaderIds[n].lastSeenUsec = now;
            g_shaderIds[n].seenCount = 1;
            g_shaderIds[n].contentSum = contentSum;
            g_shaderIds[n].contentValid = contentValid;
            g_shaderIdCount = n + 1;
        }
        LeaveCriticalSection(&g_shaderIdLock);
    }
}

// Entry-only: reads param_2 (the shader identity/tag), then jumps straight
// to the trampoline. No return-address hijack, no timing - deliberately as
// close to a no-op as possible given the SEH prologue this sits in front of.
__declspec(naked) void Detour_a957a0_observe(void)
{
    __asm {
        push ecx
        push edx
        mov eax, [esp + 16]     ; original [esp+8] = param_2, before our 2 pushes
        push eax
        call OnShaderCreate_C
        add esp, 4
        pop edx
        pop ecx
        jmp dword ptr [g_trampoline_a957a0_observe]
    }
}

#define SHADER_CREATE_RVA (0x00a957a0 - 0x00400000)
#define SHADER_CREATE_PATCH_LEN 5 // PUSH EBP; MOV EBP,ESP; PUSH -1 - exactly 3 whole instructions

static int InstallShaderIdentityHook(void)
{
    InitializeCriticalSection(&g_shaderIdLock);

    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = base + SHADER_CREATE_RVA;

    unsigned char saved[SHADER_CREATE_PATCH_LEN];
    memcpy(saved, target, SHADER_CREATE_PATCH_LEN);

    unsigned char *tramp = (unsigned char *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return 0;
    memcpy(tramp, saved, SHADER_CREATE_PATCH_LEN);
    tramp[SHADER_CREATE_PATCH_LEN] = 0xE9;
    *(int *)(tramp + SHADER_CREATE_PATCH_LEN + 1) =
        (int)(target + SHADER_CREATE_PATCH_LEN) - (int)(tramp + SHADER_CREATE_PATCH_LEN + 5);
    g_trampoline_a957a0_observe = tramp;

    DWORD oldProtect;
    if (!VirtualProtect(target, SHADER_CREATE_PATCH_LEN, PAGE_EXECUTE_READWRITE, &oldProtect)) return 0;
    target[0] = 0xE9;
    *(int *)(target + 1) = (int)(void *)Detour_a957a0_observe - (int)(target + 5);
    VirtualProtect(target, SHADER_CREATE_PATCH_LEN, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, SHADER_CREATE_PATCH_LEN);
    return 1;
}

