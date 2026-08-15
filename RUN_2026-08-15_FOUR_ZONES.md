# Four-zone stutter review — 2026-08-15

The most complete stutter measurement taken on this project. Kept as a
standalone reference because it is the baseline every future claim should be
compared against, and because the diagnostics that produced it were switched
off for release immediately afterwards.

- **Route:** Luxerion → Wild Lands → Dead Dunes → Yuusnaan, on foot via the
  highways. No loading screens after the initial spawn.
- **Duration:** ~23 minutes, 85,030 marked gameplay frames.
- **Log:** `_logs/version_hook_20260815_bigrun_4zones.log` (14.9 MB, in the
  game folder, not the repo).
- **Marks:** run start + one at each zone change, via Optimization → Mark Log.
- **Analysis:** `python tools/analyze_run.py <log>`

## Configuration under test

```
ThresholdUs=20000        watchdog armed at 20ms (itself a mild stutter source)
TargetFpsX100=6000       60fps cap, UnlockFramerate=1
ForceImmediatePresent=1  VSYNC OFF - see the caveat at the bottom
ShadowMapRes=8192  ShadowSplitNearPct=300  ShadowSplitFarPct=200
CompactorDefer=1  CompactorBudgetUs=2000  CompactorCooldown=4
SsaaScale=200  SsaaOutputRes=1  MsaaSamples=0  FxaaOff=1
DiscardFix=1  ShaderThrottle=1  ReadPace=1  StagingUpload/Surface/Cube=1
GpuSyncSkip=1  SimDeltaFix=1  ForceStdD3D9=1  HDTexPush=1
```

## Per-zone results

| segment | frames | over 20ms | rate | captures | 30ms+ | stall |
|---|---|---|---|---|---|---|
| preamble (boot/load) | 3,890 | 40 | 1.03% | 45 | 10 | 1,375ms |
| #1 Luxerion | 15,973 | 65 | 0.41% | 38 | 1 | 831ms |
| #2 Wild Lands | 29,597 | 141 | 0.48% | 112 | 0 | 2,392ms |
| #3 Dead Dunes | 22,743 | 104 | 0.46% | 74 | 0 | 1,579ms |
| #4 Yuusnaan | 16,717 | 179 | **1.07%** | 108 | 1 | 2,291ms |
| **ALL MARKED** | **85,030** | **489** | **0.58%** | **332** | **2** | **7,093ms** |

## Cause ratios — GAMEPLAY (332 captures)

| cause | share | median | worst |
|---|---|---|---|
| **Heap compactor** | **73.8%** | 21.1ms | 23.3ms |
| Engine sync wait (own workers) | 9.9% | 21.3ms | 23.0ms |
| Kernel wait (untraced) | 5.7% | 21.4ms | 42.7ms |
| Window message pump | 3.6% | 22.2ms | 22.8ms |
| GPU driver (AMDXN32) | 3.6% | 21.5ms | 22.9ms |
| Game code (other) | 2.4% | 21.8ms | 22.8ms |
| D3D9 runtime | 0.6% | 21.4ms | 21.4ms |
| Class loader (cause 3) | **0.3%** | 21.9ms | 21.9ms |
| D3DX texture decode | **0%** | — | — |

Across all nine marked logs of the day (610 captures, ~122,000 frames) the
ratios are the same shape: compactor 64.8%, sync wait 13.4%, kernel wait
8.0%, message pump 4.9%, driver 4.4%, game code 2.8%, D3D9 1.5%, loader 0.2%.

## Cause ratios — LOADING/TRANSITIONS (496 captures, all logs)

A different world, and the reason analysis must always slice at the mark:

| cause | share | median | worst |
|---|---|---|---|
| GPU driver (shader creation) | 20.0% | 22.2ms | 62.9ms |
| Kernel wait | 15.5% | 21.3ms | 62.7ms |
| D3DX texture decode | 13.5% | 21.3ms | 23.1ms |
| Engine sync wait | 12.9% | **27.6ms** | **97.2ms** |
| Class loader (cause 3) | 10.5% | 22.2ms | 60.8ms |
| Game code | 8.9% | 21.3ms | 61.0ms |
| Heap compactor | 6.5% | 21.5ms | 23.3ms |
| Message pump / D3D9 / CRT | ~12% | ~21ms | ~61ms |

36,760 loading frames produced 523 over-threshold frames (1.42%) and **91
events over 30ms** — versus 2 in 85,030 gameplay frames. A loading frame is
~3x more likely to stutter and its stutters run 2-4x heavier. Of ~21s total
stall measured across the day, loading contributed 13.5s from a third of the
frames.

## Findings

### 1. Gameplay stutter is now a one-cause story, and a mild one

The compactor is ~74% of gameplay captures and no other family reaches 10%.
But every gameplay median is ~21ms against a 16.7ms budget — a 4-6ms
overshoot — and only 2 events in 85,030 frames exceeded 30ms. Frequent but
faint, and concentrated in one subsystem.

### 2. "The game runs worse when you don't fast travel" — confirmed, measured

Compactor call cadence is flat across zones (~7,100/min), but deferral
engagement climbs monotonically:

| segment | passes deferred/min | worst pass | over-20ms rate |
|---|---|---|---|
| #1 Luxerion | 100 | 9,728µs | 0.41% |
| #2 Wild Lands | 103 | 11,755µs | 0.48% |
| #3 Dead Dunes | 120 | 9,339µs | 0.46% |
| #4 Yuusnaan | **330** | 9,503µs | **1.07%** |

Mechanism: heap fragmentation accumulates across a no-loading-screen
traverse. A fast-travel load tears down and rebuilds the zone heap — an
implicit defrag. On foot there is no reset, so churn compounds and the
compactor storms 3x as often by the end, at the same per-pass cap.

**Yuusnaan is probably not the worst zone — it was simply last.** Untested
prediction: run the same four zones in reverse; if Luxerion then measures
worst, it is accumulation rather than the zone.

**Open lever this suggests:** nothing resets the heap on foot. Triggering the
engine's own full compaction during a quiet moment (menu open, cutscene)
would give highway players the defrag fast-travellers get for free.

### 3. The compactor deferral holds up on a long route

The earlier "Ruffian → Luxerion felt worse with deferral on" concern is not
borne out: 2 events over 30ms in 23 minutes, neither of them the compactor,
worst compactor capture 23.3ms. Severity stays capped even under end-of-run
fragmentation load. What the deferral cannot do is stop storms becoming more
frequent as the heap degrades.

### 4. The Luxerion "big red" stutter is partly ours

15:42:30, 42.5ms, the only heavy event in Luxerion. The record reads
`reads=8/512KB pace_usec=42595`, and 512KB at the ReadPace limiter's 12MB/s
is 42.6ms — the arithmetic is exact. A mid-gameplay 512KB read was paced
across 42ms (the bulk-load bypass never fired; 512KB is far under its 6MB
threshold) and the frame stalled waiting for it. External disk contention
(a video player reaching its next file) plausibly explains the *trigger*;
the *magnitude* is our own pacer.

Not changed: ReadPace is A/B-confirmed necessary for chunk-load stutters, and
one 42ms event in 23 minutes does not justify touching a confirmed fix
without a plan. Filed as a tuning question — the bypass threshold, or a
size-based exemption for small mid-gameplay reads.

The second 42ms event (16:04:38, Yuusnaan) is `reads=0 pace_usec=0`, a pure
wait on the same message-pump stack — consistent with external interference.

### 5. Cause 3 (class loader) is a loading-screen phenomenon, full stop

ONE capture in 23 minutes of gameplay, 0.3%. It is 10.5% of loading
stutters. Unparked and measured; it simply does not occur in play.

## Caveat that limits how far this generalises

**This run had vsync OFF** (`ForceImmediatePresent=1`) on a FreeSync display.
That is not the shipping default — `ForceImmediatePresent` defaults to 0, and
the game requests `D3DPRESENT_INTERVAL_DEFAULT`, which the D3D9 spec defines
as identical to `INTERVAL_ONE`. Windowed presentation means DWM paces it, so
the driver-level "vsync off" toggle does not apply either.

Consequence: every gameplay capture here sits at 21-23ms, i.e. "missed the
16.7ms budget by a few ms". Under a 60Hz compositor there is no 21ms frame —
it waits a full extra interval and becomes **33.3ms**. The median frame in
this run is already 16.5-17.0ms, right on the deadline.

So these numbers are the **best case**: VRR + vsync off. A fixed-refresh
user with the game's vsync intact would likely see the same causes in the
same ratios but with severity roughly doubled.

**This is mechanism, not measurement.** PROGRESS.md contains a *retracted*
vsync-quantisation claim (asserted from a short stationary run against a full
town-run baseline, correctly rejected). The standing methodology note
requires any vsync claim to use the same route as its baseline — which now
exists. The test: repeat this exact route with `ForceImmediatePresent=0`.
Prediction: cause ratios barely move; 30ms+ events jump from 2 toward the
full 332.
