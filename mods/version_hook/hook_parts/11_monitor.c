// ---- Timer resolution probe + override -----------------------------------
// Testing a specific hypothesis about the COLD-pass stutters. Categorising
// the cold pass's ntdll waits by what they are actually blocked on gave
// 27 "main thread asleep INSIDE the game's own frame limiter" vs only 20
// genuine AMD driver waits - i.e. nearly half were never a driver problem at
// all. A normal frame here is ~17ms and consists almost entirely of that
// limiter sleep, so a frame only crosses the 20ms threshold this way if a
// single Sleep(1) inside FUN_00ac3040's spin overshoots badly.
//
// Windows' DEFAULT timer resolution is ~15.6ms, which would make Sleep(1)
// block for up to that long - exactly the overshoot signature. Nothing in
// this mod has ever called timeBeginPeriod (verified: zero references), and
// while RTSS is loaded and probably raises it process-wide, that has never
// been MEASURED here. So: query the real value rather than assume it either
// way. NtQueryTimerResolution reports in 100ns units, so 1ms == 10000 and
// the 15.6ms default == 156250.
//
// This probe is deliberately decisive in BOTH directions: if the current
// resolution already reads ~1ms, the whole hypothesis is dead on the spot
// and TimerRes cannot help, no test run needed. Only if it reads coarse is
// the override worth A/B testing.
//
// timeBeginPeriod/timeEndPeriod are a matched pair and safe to call at
// runtime, so this can hot-toggle. winmm is loaded dynamically to keep the
// existing link line unchanged and to stay out of DllMain's loader lock.
// RETIRED (ENABLE_TIMER_RES). The probe above was decisive in the direction
// that kills it: the system already reads 1.0000ms, so the override has
// nothing to raise and the Sleep-overshoot hypothesis is dead. Never produced
// a test run.
#if ENABLE_TIMER_RES
typedef LONG (WINAPI *PFN_NtQueryTimerResolution)(PULONG, PULONG, PULONG);
typedef UINT (WINAPI *PFN_timePeriod)(UINT);
static PFN_NtQueryTimerResolution g_ntQueryTimerRes = NULL;
static PFN_timePeriod g_timeBeginPeriod = NULL;
static PFN_timePeriod g_timeEndPeriod = NULL;
static volatile LONG g_timerResEnabled;   // tentative def near the top too
static LONG g_timerResApplied = 0;   // monitor-thread only, no atomics needed

static void ReportAndApplyTimerResolution(void)
{
    static int resolved = 0;
    if (!resolved) {
        resolved = 1;
        HMODULE hNt = GetModuleHandleA("ntdll.dll");
        if (hNt) g_ntQueryTimerRes =
            (PFN_NtQueryTimerResolution)GetProcAddress(hNt, "NtQueryTimerResolution");
        HMODULE hWinmm = LoadLibraryA("winmm.dll");
        if (hWinmm) {
            g_timeBeginPeriod = (PFN_timePeriod)GetProcAddress(hWinmm, "timeBeginPeriod");
            g_timeEndPeriod   = (PFN_timePeriod)GetProcAddress(hWinmm, "timeEndPeriod");
        }
    }

    if (g_timerResEnabled && !g_timerResApplied && g_timeBeginPeriod) {
        g_timeBeginPeriod(1);
        g_timerResApplied = 1;
        LogLine("[timer] TimerRes ON - timeBeginPeriod(1) applied");
    } else if (!g_timerResEnabled && g_timerResApplied && g_timeEndPeriod) {
        g_timeEndPeriod(1);
        g_timerResApplied = 0;
        LogLine("[timer] TimerRes OFF - timeEndPeriod(1), resolution released");
    }

    // Report the ACTUAL achieved resolution, not what we asked for - another
    // process can be holding a finer period, and only the real value explains
    // the limiter's behaviour.
    if (g_ntQueryTimerRes) {
        ULONG mn = 0, mx = 0, cur = 0;
        if (g_ntQueryTimerRes(&mn, &mx, &cur) == 0) {
            static ULONG lastCur = 0;
            if (cur != lastCur) {
                lastCur = cur;
                char line[192];
                sprintf(line, "[timer] actual resolution now %.4fms (coarsest=%.4fms finest=%.4fms) - "
                              "Sleep(1) can overshoot up to this much",
                        cur / 10000.0, mn / 10000.0, mx / 10000.0);
                LogLine(line);
            }
        }
    }
}
#endif  // ENABLE_TIMER_RES

static DWORD WINAPI MonitorThread(LPVOID param)
{
    (void)param;
    for (;;) {
        Sleep(500);
#if ENABLE_SURFACE_DIAG
        // F9 capture poll lives here as well as in the panel's timer: the
        // panel's timer only runs while that window is OPEN, and the first
        // F9 attempt logged nothing at all, which a closed panel would
        // explain exactly. This thread always runs.
        {
            static int f9Down = 0;
            int f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            if (f9 && !f9Down) {
                g_captureRequest = 1;
                LogLine("[capture] F9 (monitor thread) - full-frame capture armed");
            }
            f9Down = f9;
        }
#endif  // ENABLE_SURFACE_DIAG
#if ENABLE_TIMER_RES
        ReportAndApplyTimerResolution();
#endif
        // REMOVED 2026-08-13: the one-shot "rewrite the ini so every key is
        // visible" used to run here, on the monitor thread's FIRST tick.
        //
        // Moving it here from startup was supposed to make it safe. It did
        // not: every failing run dies at exactly this point in the log - the
        // first monitor window - and the crashes are access violations on the
        // MAIN thread inside the HD GUI mod, i.e. concurrent with this write
        // rather than caused by it directly. Whether the mechanism is
        // contention or timing, a file write is not worth putting next to
        // another mod's initialisation.
        //
        // The ini is already complete on disk, and SaveConfig still runs
        // whenever a setting actually changes, so nothing is lost except the
        // first-launch-after-update case - which is worth far less than a
        // reliable startup. If it is ever wanted back, it belongs somewhere
        // provably late (first rendered frame, not first monitor tick).
        ApplyShadowResolution();
        // Same cadence and same risk profile as ApplyShadowResolution: writes
        // an engine settings field from this thread and lets the engine's own
        // change detector reallocate. That pattern is already shipping.
        ApplySsaaScale();
#if ENABLE_SCALING_MODE
        ApplyScalingMode();
#endif
#if ENABLE_SHADER_DIAG
        // Rank EVERY pixel shader by draws in the last window. Cumulative
        // counts made rank #1 wander between near-equal shaders (it flashed
        // on random objects); a per-window delta ranks what is actually being
        // drawn in the current view, which is the whole point of the walk.
        {
            LONG n = g_psMapCount, i, r;
            if (n > PS_MAP_MAX) n = PS_MAP_MAX;
            for (i = 0; i < n; i++) {
                LONG d = g_psMap[i].draws;
                g_psMap[i].recent = d - g_psMap[i].prevDraws;
                g_psMap[i].prevDraws = d;
            }
            LONG prevBest = 0x7FFFFFFF, count = 0;
            for (r = 0; r < PS_RANK_MAX; r++) {
                LONG cur = -1, curDraws = 0;
                for (i = 0; i < n; i++) {
                    LONG d = g_psMap[i].recent;
                    if (d <= 0 || d >= prevBest) continue;
                    if (d > curDraws) { curDraws = d; cur = i; }
                }
                if (cur < 0) break;
                g_psRankIdx[r] = cur;
                prevBest = curDraws;
                count = r + 1;
            }
            g_psRankCount = count;
            // Mark which ranked shaders have a cutout variant, so the label
            // can say whether A2C is even applicable to what is highlighted.
            for (r = 0; r < count; r++) {
                LONG idx = g_psRankIdx[r];
                g_psMap[idx].hasVariant = 0;
                LONG vn = g_a2cVariantCount;
                if (vn > A2C_MAX) vn = A2C_MAX;
                for (i = 0; i < vn; i++)
                    if (g_a2cVariants[i].orig == g_psMap[idx].obj) {
                        g_psMap[idx].hasVariant = 1;
                        break;
                    }
            }
        }
#endif  // ENABLE_SHADER_DIAG
        ReportAndClampSleepGranularity();
        FlushLog();   // see LogLine: buffered writes, flushed here instead
#if ENABLE_FRAMETIME_DUMP
        // Frametime burst: arm on the rising edge, dump once full.
        {
            static LONG lastFtState = 0;
            LONG now = g_logFrameTimes;
            if (now && !lastFtState) {
                g_ftCount = 0;
                g_ftDumped = 0;
                LogLine("[ft] ---- frametime capture armed (toggle switched on) ----");
            }
            // Dump when the buffer fills OR when the toggle is switched off.
            // The first version only dumped on "full", so a capture ended
            // early - by unticking it or closing the game before 2048 frames
            // (~18s at 110fps) - produced NOTHING. That silently cost a
            // second capture. A partial run is still perfectly usable for a
            // period measurement, so it should never be thrown away.
            LONG fell = (!now && lastFtState);
            lastFtState = now;
            if (!g_ftDumped && g_ftCount > 0 &&
                (fell || (now && g_ftCount >= FT_CAPTURE_MAX))) {
                LONG total = g_ftCount;
                g_ftDumped = 1;
                char l[420];
                sprintf(l, "[ft] ---- dumping %ld samples (%s) ----", total,
                        fell ? "stopped early - toggle switched off" : "buffer full");
                LogLine(l);
                for (LONG i = 0; i < total; i += 20) {
                    int o = sprintf(l, "[ft] %4ld:", i);
                    for (LONG j = i; j < i + 20 && j < total; j++)
                        o += sprintf(l + o, " %ld", g_ftBuf[j]);
                    LogLine(l);
                }
                // Every series is the same frames in the same order, so they
                // line up index-for-index and can be correlated directly.
                struct { const char *tag; LONG *buf; } cols[] = {
                    { "fs", g_ftShadow }, { "fa", g_ftAllocs },
                    { "fr", g_ftReads },  { "fw", g_ftWfso },
                    { "fp", g_ftPacer },
                };
                for (int c = 0; c < 5; c++) {
                    for (LONG i = 0; i < total; i += 20) {
                        int o = sprintf(l, "[%s] %4ld:", cols[c].tag, i);
                        for (LONG j = i; j < i + 20 && j < total; j++)
                            o += sprintf(l + o, " %ld", cols[c].buf[j]);
                        LogLine(l);
                    }
                }
                LogLine("[ft] ---- end: ft=engine tick us, fs=DRAW_SHADOW cpu us, "
                        "fa=allocs, fr=file reads, fw=WFSO calls, fp=FUN_00ac3040 us ----");
            }
        }
#endif  // ENABLE_FRAMETIME_DUMP
#if ENABLE_TALK_TIMER
        ApplyTalkTimerScale();
#endif
#if ENABLE_CASCADE_HUNT
        // The 4000-entry constant dump filled up during startup and menus, so
        // it never reached actual gameplay and captured no shadow pass at all.
        // Restarting the counter every time the toggle is switched ON turns it
        // into a "capture from here" button: tick it while outdoors and the
        // window covers that moment instead of the title screen.
        {
            static LONG lastLogState = 0;
            LONG now = g_logShaderConsts;
            if (now && !lastLogState) {
                g_constSeq = 0;
                LogLine("[seq] ---- capture restarted (toggle switched on) ----");
            }
            lastLogState = now;
        }
#endif
        char line[256];

        // Frame time first, so every window in the log opens with the number
        // that actually matters.
        if (!g_mainThreadHandle && g_mainThreadId) {
            g_mainThreadHandle = OpenThread(
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                FALSE, (DWORD)g_mainThreadId);
            sprintf(line, "[watchdog] main thread tid=%ld handle=%s",
                    g_mainThreadId, g_mainThreadHandle ? "OK" : "FAILED");
            LogLine(line);
        }

        // Bulk-load detection: sustained high read volume = loading screen,
        // intermittent = traversal streaming. See BULK_LOAD_BYTES_PER_WINDOW.
        {
            static LONGLONG prevReadBytes = 0;
            static int bulkStreak = 0, quietStreak = 0;
            LONGLONG nowBytes = g_readFileBytes;
            LONGLONG delta = nowBytes - prevReadBytes;
            prevReadBytes = nowBytes;
            if (delta >= BULK_LOAD_BYTES_PER_WINDOW) { bulkStreak++; quietStreak = 0; }
            else { quietStreak++; bulkStreak = 0; }
            if (!g_bulkLoadActive && bulkStreak >= 2) {
                g_bulkLoadActive = 1;
                sprintf(line, "[probe] bulk load detected (%lldKB this window) - throttles BYPASSED",
                        delta / 1024);
                LogLine(line);
            } else if (g_bulkLoadActive && quietStreak >= 2) {
                g_bulkLoadActive = 0;
                LogLine("[probe] bulk load ended - throttles re-engaged");
            }
        }

        {
            static LONG prevCreate = 0, prevDestroy = 0;
            LONG cNow = g_createTexCount, dNow = g_texDestroyCount;
            LONG dC = cNow - prevCreate, dD = dNow - prevDestroy;
            prevCreate = cNow; prevDestroy = dNow;
            LONG kb = InterlockedExchange(&g_uploadKB, 0);
            if (dC > 0 || dD > 0 || kb > 0) {
                LONG kbMain = InterlockedExchange(&g_uploadKBMain, 0);
                sprintf(line, "[probe] tex churn: created=%ld destroyed=%ld (cum %ld/%ld) | uploaded=%ldKB (mainthread=%ldKB) this window",
                        dC, dD, cNow, dNow, kb, kbMain);
                LogLine(line);
            }
        }

        LONG clamped = InterlockedExchange(&g_deadlineClampCount, 0);
        if (clamped > 0) {
            sprintf(line, "[probe] frame-limiter deadline clamped %ld time(s) this window (aftershock prevented)", clamped);
            LogLine(line);
        }

        LONG maxFrame = InterlockedExchange(&g_maxFrameUsec, 0);
        LONG frames = InterlockedExchange(&g_frameCount, 0);
        LONG frameSum = InterlockedExchange(&g_frameSumUsec, 0);
        LONG paceDelay = InterlockedExchange(&g_readPaceDelayUsec, 0);
        LONG csWait = InterlockedExchange(&g_mainCsWaitTotalUsec, 0);
        LONG csWaitN = InterlockedExchange(&g_mainCsWaitCount, 0);
        LONG wfsoWait = InterlockedExchange(&g_mainWfsoWaitTotalUsec, 0);
        sprintf(line, "[monitor] FRAME: frames=%ld avg_usec=%.1f WORST_usec=%ld | readpace=%ld mainCS=%ldus/%ld mainWFSO=%ldus",
                frames, frames > 0 ? (double)frameSum / (double)frames : 0.0, maxFrame,
                paceDelay, csWait, csWaitN, wfsoWait);
        LogLine(line);

        LONG rateLimited = InterlockedExchange(&g_stutterRateLimited, 0);
        if (rateLimited > 0) {
            sprintf(line, "[watchdog] %ld slow frame(s) this window had NO capture attempt - "
                          "rate limit (%d/sec) saturated, records/stall totals are an UNDERCOUNT here",
                    rateLimited, STUTTER_MAX_CAPTURES_PER_SEC);
            LogLine(line);
        }

        // Frame-time distribution for this window. Percentiles come straight
        // out of the histogram, and the "below 60fps" counts are the
        // comprehensive sub-60 list - every frame over 16.67ms, regardless of
        // whether it was severe enough for the watchdog to bother capturing.
        {
            LONG hist[FRAME_HIST_BUCKETS];
            LONG total = 0;
            for (int b = 0; b < FRAME_HIST_BUCKETS; b++) {
                hist[b] = InterlockedExchange(&g_frameHist[b], 0);
                total += hist[b];
            }
            if (total > 0) {
                LONG p50=0, p90=0, p99=0, acc=0;
                LONG thr = g_stutterThresholdUsec;
                LONG n60=0, n50=0, n30=0;   // >threshold, >1.2x, >2x
                for (int b = 0; b < FRAME_HIST_BUCKETS; b++) {
                    acc += hist[b];
                    LONG usec_hi = (b + 1) * 500;
                    if (!p50 && acc * 100 >= total * 50) p50 = usec_hi;
                    if (!p90 && acc * 100 >= total * 90) p90 = usec_hi;
                    if (!p99 && acc * 100 >= total * 99) p99 = usec_hi;
                    if (usec_hi > thr) n60 += hist[b];
                    if (usec_hi > thr * 6 / 5) n50 += hist[b];
                    if (usec_hi > thr * 2) n30 += hist[b];
                }
                sprintf(line, "[frametime] n=%ld p50=%ld p90=%ld p99=%ld | thr=%ldus over=%ld (%.1f%%) over1.2x=%ld over2x=%ld",
                        total, p50, p90, p99, thr, n60, total ? n60*100.0/total : 0.0, n50, n30);
                LogLine(line);
                g_liveP50 = p50; g_liveP99 = p99; g_liveOver16 = n60;
                g_liveFrames = total; g_liveWorst = maxFrame;
            }
        }

        // Drain whatever the stutter watchdog captured. Logged as Ghidra VAs
        // (runtime address - module base + 0x00400000) so they can be pasted
        // straight into Ghidra with no per-session ASLR arithmetic.
        for (int r = 0; r < MAX_STUTTER_RECS; r++) {
            StutterRec *rec = &g_stutterRecs[r];
            if (!rec->ready) continue;

            char eipStr[160];
            DescribeAddr(rec->eip, eipStr);
            char head[512];
            sprintf(head, "[stutter] elapsed_usec=%ld EIP=%s reads=%ld/%ldKB pace_usec=%ld allocs=%ld",
                    rec->elapsedUsec, eipStr, rec->readCountInFrame, rec->readKbInFrame,
                    rec->paceUsecInFrame, rec->allocsInFrame);
            LogLine(head);

            if (rec->csWaitUsec || rec->wfsoWaitUsec) {
                const char *ownerName = "?";
                EnterCriticalSection(&g_threadNameLock);
                for (LONG j = 0; j < g_threadNameCount; j++) {
                    if (g_threadNames[j].threadId == rec->csOwnerTid) { ownerName = g_threadNames[j].name; break; }
                }
                LeaveCriticalSection(&g_threadNameLock);
                sprintf(head, "[stutter]   BLOCKED cs=%08X held_by=tid %lu (%s) for %ld us | wfso handle=%08X for %ld us",
                        rec->csPtr, rec->csOwnerTid, ownerName, rec->csWaitUsec,
                        rec->wfsoHandle, rec->wfsoWaitUsec);
                LogLine(head);
            }

            char buf[1024];
            int so = 0;
            buf[0] = 0;
            for (LONG k = 0; k < rec->stackCount && so < 800; k++) {
                char one[160];
                DescribeAddr(rec->stack[k], one);
                so += sprintf(buf + so, " %s", one);
            }
            sprintf(head, "[stutter]   ebp:%s", buf);
            LogLine(head);

            so = 0;
            buf[0] = 0;
            for (LONG k = 0; k < rec->scanCount && so < 800; k++) {
                so += sprintf(buf + so, " %08X", rec->scan[k] - g_mainModBase + 0x00400000);
            }
            sprintf(head, "[stutter]   scan:%s", buf);
            LogLine(head);

            rec->ready = 0;
        }
        for (int i = 0; i < NUM_FNS; i++) {
            HookedFunc *hf = &g_funcs[i];
            LONG count = hf->durationCount;
            LONGLONG sum = hf->durationSumCycles;

            LONG windowCount = count - g_prevDurCount[i];
            LONGLONG windowSum = sum - g_prevDurSum[i];
            g_prevDurCount[i] = count;
            g_prevDurSum[i] = sum;

            double avgUsec = (windowCount > 0 && g_cyclesPerUsec > 0.0)
                ? ((double)windowSum / (double)windowCount) / g_cyclesPerUsec
                : 0.0;
            LONG maxUsec = InterlockedExchange(&hf->maxUsec, 0);
            sprintf(line, "[monitor] %s: calls_in_window=%ld avg_duration_usec=%.2f max_duration_usec=%ld total_calls=%ld",
                    hf->name, windowCount, avgUsec, maxUsec, hf->callCount);
            LogLine(line);
        }

        // WaitForSingleObject: report TOTAL time spent blocked across all
        // threads in this window (sum, not average) - this is the
        // aggregate "how much wall-clock-equivalent time did the process
        // spend waiting on kernel sync objects" signal, most relevant to
        // the job-system-contention hypothesis.
        LONG wCount = g_wfsoCallCount;
        LONGLONG wSum = g_wfsoSumCycles;
        LONG wWindowCount = wCount - g_prevWfsoCount;
        LONGLONG wWindowSum = wSum - g_prevWfsoSum;
        g_prevWfsoCount = wCount;
        g_prevWfsoSum = wSum;
        double totalUsec = (g_cyclesPerUsec > 0.0) ? (double)wWindowSum / g_cyclesPerUsec : 0.0;
        sprintf(line, "[monitor] WaitForSingleObject: calls_in_window=%ld total_blocked_usec=%.1f total_calls=%ld",
                wWindowCount, totalUsec, wCount);
        LogLine(line);

        // Per-thread windowed breakdown - which thread(s) the blocking
        // time above actually belongs to.
        for (int i = 0; i < MAX_WFSO_THREADS; i++) {
            WfsoThreadSlot *slot = &g_wfsoThreads[i];
            if (slot->threadId == 0) continue;
            LONG tCount = slot->callCount;
            LONGLONG tSum = slot->sumCycles;
            LONG tWindowCount = tCount - g_prevWfsoThreadCount[i];
            LONGLONG tWindowSum = tSum - g_prevWfsoThreadSum[i];
            g_prevWfsoThreadCount[i] = tCount;
            g_prevWfsoThreadSum[i] = tSum;
            if (tWindowCount <= 0) continue;
            double avgUsec = (g_cyclesPerUsec > 0.0)
                ? ((double)tWindowSum / (double)tWindowCount) / g_cyclesPerUsec
                : 0.0;
            sprintf(line, "[monitor]   thread %ld: calls_in_window=%ld avg_blocked_usec=%.2f",
                    slot->threadId, tWindowCount, avgUsec);
            LogLine(line);
        }

        LogD3DWindow();

        LONG dispatchTotal = g_loaderDispatchCount;
        LONG dispatchWindow = dispatchTotal - g_prevDispatchCount;
        g_prevDispatchCount = dispatchTotal;
        sprintf(line, "[monitor] LoaderDispatch: processed_in_window=%ld total=%ld",
                dispatchWindow, dispatchTotal);
        LogLine(line);

        LONG pfAllocTotal = g_prefetchedAllocCount;
        LONGLONG pfByteTotal = g_prefetchedByteCount;
        LONG pfAllocWindow = pfAllocTotal - g_prevPrefetchedAllocCount;
        LONGLONG pfByteWindow = pfByteTotal - g_prevPrefetchedByteCount;
        g_prevPrefetchedAllocCount = pfAllocTotal;
        g_prevPrefetchedByteCount = pfByteTotal;
        sprintf(line, "[monitor] InDispatchAllocs: allocs_in_window=%ld bytes_in_window=%lld total_allocs=%ld total_bytes=%lld",
                pfAllocWindow, pfByteWindow, pfAllocTotal, pfByteTotal);
        LogLine(line);

        // Sync vs overlapped ReadFile split - if this ever shows overlapped
        // calls in real numbers, per-call duration stops meaning "blocked
        // this long" and the real wait would be hiding in a later
        // GetOverlappedResult/WaitForSingleObject instead.
        LONG syncTotal = g_readFileSyncCount, ovlTotal = g_readFileOverlappedCount;
        sprintf(line, "[monitor] ReadFile mode: sync_total=%ld overlapped_total=%ld", syncTotal, ovlTotal);
        LogLine(line);

        // Distinct compiled shaders vs total compiles vs repeats - answers
        // directly whether reproducible stutters are the SAME shader
        // recompiling (cache eviction - repeats climbing) or genuinely new
        // ones each time (permutation explosion - distinct climbs, repeats
        // stay near zero). [shader] REPEAT lines above give the per-event
        // detail; this is the running aggregate.
        sprintf(line, "[monitor] ShaderCompiles: total=%ld distinct=%ld repeats=%ld",
                g_shaderCompileTotal, g_shaderIdCount, g_shaderCompileRepeats);
        LogLine(line);

        sprintf(line, "[monitor] ShaderBudget: min_seen=%ld max_seen=%ld engaged_total=%ld",
                g_shaderBudgetMinSeen == 0x7FFFFFFF ? -1 : g_shaderBudgetMinSeen,
                g_shaderBudgetMaxSeen, g_shaderBudgetEngagedCount);
        LogLine(line);

        for (int b = 0; b < 2; b++) {
            LONG c = InterlockedExchange(&g_allocDurCount[b], 0);
            LONGLONG s = InterlockedExchange64((LONGLONG *)&g_allocDurSumUsec[b], 0);
            LONG mx = InterlockedExchange(&g_allocDurMaxUsec[b], 0);
            if (c <= 0) continue;
            sprintf(line, "[monitor] AllocatorDuration[%s]: calls=%ld avg_usec=%.2f max_usec=%ld",
                    b == 0 ? "MAIN" : "other", c, (double)s / (double)c, mx);
            LogLine(line);
        }

        // ---- v6: full allocation census, per thread ----------------------
        // The decisive number: which thread actually allocates the bulk of
        // the bytes during a chunk-load stutter. Attempt 5 only ever saw the
        // loader-dispatch slice of this.
        LONG srcCount[NUM_ALLOC_SRC] = { 0 };
        LONGLONG srcBytes[NUM_ALLOC_SRC] = { 0 };
        for (int i = 0; i < MAX_ALLOC_THREADS; i++) {
            AllocThreadSlot *slot = &g_allocThreads[i];
            if (slot->threadId == 0) continue;

            LONG wc[NUM_ALLOC_SRC];
            LONGLONG wb[NUM_ALLOC_SRC];
            LONG anyCount = 0;
            for (int s = 0; s < NUM_ALLOC_SRC; s++) {
                LONG c = slot->count[s];
                LONGLONG b = slot->bytes[s];
                wc[s] = c - g_prevAllocThreadCount[i][s];
                wb[s] = b - g_prevAllocThreadBytes[i][s];
                g_prevAllocThreadCount[i][s] = c;
                g_prevAllocThreadBytes[i][s] = b;
                anyCount += wc[s];
                srcCount[s] += wc[s];
                srcBytes[s] += wb[s];
            }
            if (anyCount <= 0) continue;

            const char *name = "?";
            EnterCriticalSection(&g_threadNameLock);
            for (LONG j = 0; j < g_threadNameCount; j++) {
                if (g_threadNames[j].threadId == (DWORD)slot->threadId) { name = g_threadNames[j].name; break; }
            }
            LeaveCriticalSection(&g_threadNameLock);

            char detail[256];
            int off = 0;
            for (int s = 0; s < NUM_ALLOC_SRC; s++) {
                if (wc[s] <= 0) continue;
                off += sprintf(detail + off, " %s=%ld/%lldB", g_allocSrcNames[s], wc[s], wb[s]);
            }
            sprintf(line, "[monitor]   alloc thread %ld (%s%s):%s",
                    slot->threadId, name,
                    (slot->threadId == g_mainThreadId) ? ",MAIN" : "",
                    detail);
            LogLine(line);
        }
        int coff = 0;
        char ctotals[256];
        for (int s = 0; s < NUM_ALLOC_SRC; s++) {
            coff += sprintf(ctotals + coff, " %s=%ld/%lldB", g_allocSrcNames[s], srcCount[s], srcBytes[s]);
        }
        sprintf(line, "[monitor] AllocCensus:%s", ctotals);
        LogLine(line);

#if ENABLE_ALLOCATOR_WARM
        // Unconditional, unlike most of these - so it has to be gated rather
        // than left to a zero counter, or it would log an all-zeros line every
        // window forever.
        LONG weTotal = g_warmEnqueued, wdTotal = g_warmDone;
        LONG wdropTotal = g_warmDropped;
        LONGLONG wbTotal = g_warmBytes;
        sprintf(line, "[monitor] Warmer: enqueued_in_window=%ld warmed_in_window=%ld bytes_in_window=%lld dropped_in_window=%ld backlog=%d",
                weTotal - g_prevWarmEnqueued, wdTotal - g_prevWarmDone,
                wbTotal - g_prevWarmBytes, wdropTotal - g_prevWarmDropped,
                g_warmCount);
        g_prevWarmEnqueued = weTotal;
        g_prevWarmDone = wdTotal;
        g_prevWarmBytes = wbTotal;
        g_prevWarmDropped = wdropTotal;
        LogLine(line);
#endif

        // Distinct concrete allocator implementations behind the named-heap
        // wrapper. Newly-seen ones are logged once each; their addresses are
        // what to feed back into Ghidra to enumerate every OTHER wrapper that
        // funnels into the same implementation (i.e. the other allocation
        // paths this hook does not currently see).
        LONG implCount = g_allocImplCount;
        for (LONG i = 0; i < implCount; i++) {
            if (g_allocImpls[i].logged) continue;
            g_allocImpls[i].logged = 1;
            sprintf(line, "[allocimpl] #%ld vtable=0x%08X allocFn=0x%08X heap=0x%08X (module_rva_allocFn=0x%08X)",
                    i,
                    (unsigned int)g_allocImpls[i].vtable,
                    (unsigned int)g_allocImpls[i].allocFn,
                    (unsigned int)g_allocImpls[i].exampleHeap,
                    (unsigned int)((unsigned char *)g_allocImpls[i].allocFn - (unsigned char *)GetModuleHandleA(NULL)));
            LogLine(line);
        }
    }
}

