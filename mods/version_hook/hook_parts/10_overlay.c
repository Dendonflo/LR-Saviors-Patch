// ---- Control panel (separate Win32 window, not a D3D9 overlay) ------------
// A D3D9-rendered overlay (ImGui or similar, drawn via the already-hooked
// Present) was considered and rejected for this: it would need its own
// input capture wired through the game's own message loop, a font/vertex
// buffer setup, and stays coupled to the render pipeline this whole
// investigation has been chasing bugs in - exactly the wrong place to add
// more surface area right now. A plain top-level Win32 window is simpler on
// every axis: it gets its own independent message pump and input handling
// for free from the OS, is completely decoupled from the game's own
// rendering (so it can't be broken by anything found in this file), and
// native checkboxes are both the visual feedback AND the control - no
// separate "did my toggle take effect" step needed. Extensible for future
// mod features the same way: add another CreateWindowA checkbox/control and
// a WM_COMMAND case.
#define IDC_CHECK_BASE 1001 // one ID per entry in g_toggles: 1001, 1002, 1003...
#define IDT_SYNC_TIMER 2001
#define IDC_THRESH_EDIT 3001
#define IDC_THRESH_APPLY 3002
#define IDC_FPS_EDIT 3003
#define IDC_FPS_APPLY 3004
#define IDC_DEFER_EDIT 3005
#define IDC_DEFER_APPLY 3006
#define IDC_SHADOW_COMBO 3007
#define IDC_SPLITNEAR_COMBO 3008
#define IDC_SPLITFAR_COMBO 3009
#define IDC_TALKTIMER_COMBO 3010
// The engine's talk-entry countdown steps 0.05 per FRAME with no delta-time
// term, so at 60fps a dialogue's slot is torn down in half the wall-clock time
// the designers tuned for. 50% restores the 30fps rate. Higher values are
// offered for A/B: if 50 fixes a bug and 200 makes it much worse, that is the
// confirmation that this timer is the one involved.
static const LONG g_talkPctChoices[] = { 0, 50, 100, 200 };
static const char *g_talkPctLabels[] = {
    "Off (leave engine alone)",
    "50%  (30fps rate - the fix)",
    "100%  (unchanged)",
    "200%  (2x faster - should worsen)"
};
#define NUM_TALK_CHOICES (sizeof(g_talkPctChoices)/sizeof(g_talkPctChoices[0]))
static const LONG g_splitPctChoices[] = { 0, 100, 150, 200, 300, 400, 600 };
static const char *g_splitPctLabels[] = {
    "Off (engine default)", "100%  (unchanged)", "150%", "200%  (2x)",
    "300%  (3x)", "400%  (4x)", "600%  (6x)"
};
#define NUM_SPLIT_CHOICES (sizeof(g_splitPctChoices)/sizeof(g_splitPctChoices[0]))
// Engine's own two settings are 1024 ("Standard") and 2048 ("Advanced"), so
// the multipliers are expressed against 2048. 8192 means an 8192x16384
// cascade atlas, which is past what a lot of D3D9 drivers will allocate -
// offered, but expected to be the one that fails.
static const LONG g_shadowResChoices[] = { 0, 1024, 2048, 4096, 8192 };
static const char *g_shadowResLabels[] = {
    "Off (leave engine alone)",
    "1024  (engine Standard)",
    "2048  (engine Advanced)",
    "4096  (x2)",
    // x4 was tested: it allocates and renders, but some camera angles push
    // the GPU into heavy oscillating stutter (8192 means an 8192x16384
    // atlas, ~512MB in R32F before the depth pair). Left selectable, marked
    // so it is not mistaken for a free upgrade. x2 is the practical ceiling.
    "8192  (x4 - allocates, but stutters)"
};
#define NUM_SHADOW_CHOICES (sizeof(g_shadowResChoices)/sizeof(g_shadowResChoices[0]))

// Screen-space shadow buffer resolution, as a percentage of the SCREEN.
// Expressed against the screen rather than against the engine's own half-res
// value, so the numbers mean what they look like: 50 IS the engine default,
// 100 is full screen, 200 is 2x supersampled. (The internal descriptor
// multiplier is twice this, since the engine's own buffers start at half.)
//
// Measured response, 4K, user-tested: 25 unusable (blocky, artifacts around
// objects), 50 shimmery, 100 modestly better, 200 clearly better. Gains do
// NOT plateau at full screen - they continue into supersampling, which means
// the limit is ALIASING in the shadow term, not softness in the filter.
// VRAM is the reason to stop: the set is three buffers at the chosen size
// plus two at half of it, all 4 bytes/px.
static const LONG g_shadowBufChoices[] = { 0, 25, 50, 100, 200 };
static const char *g_shadowBufLabels[] = {
    "Off (leave engine alone)",
    "25%  (quarter screen - unusable, artifacts)",
    "50%  (half screen - ENGINE DEFAULT)",
    "100%  (full screen, ~115MB at 4K)",
    "200%  (2x supersampled, ~460MB at 4K)"
};
#define NUM_SHADOWBUF_CHOICES (sizeof(g_shadowBufChoices)/sizeof(g_shadowBufChoices[0]))
#define IDC_SHADOWBUF_COMBO 3011

// MSAA sample count. Hot-togglable: the substitution checks this per bind, so
// a change applies within a frame; a sample-count change releases and
// recreates the MS surfaces on the render thread (never from here - GUI
// thread must not touch device objects). Only engages when the in-game
// resolution equals the desktop resolution (identity latch requirement).
static const LONG g_msaaChoices[] = { 0, 2, 4, 8 };
static const char *g_msaaLabels[] = {
    "Off",
    "2x  (~130MB at 4K)",
    "4x  (~400MB at 4K)",
    "8x  (~790MB at 4K - may exceed VRAM)"
};
#define NUM_MSAA_CHOICES (sizeof(g_msaaChoices)/sizeof(g_msaaChoices[0]))
#define IDC_MSAA_COMBO 3012
#define IDC_A2C_CHECK  3013
#define IDC_FXAA_COMBO 3014

// Post-shader kill candidates - the top tap-count fullscreen-filter shaders
// from the offline dump scan. Declared here because the GUI dropdown needs
// the labels and sits earlier in this file than the shader hooks; the hash
// table is the authority, labels must stay in step. Fingerprint-by-constants
// already misfired once (ps_072C19AF matched FXAA's 1/8+1/16 thresholds and
// proved visually inert across 11,533 confirmed substitutions - a PCF/blur
// step ladder), so the dropdown + magenta footprint mode exists to identify
// the real AA pass EMPIRICALLY.
// v2 list. The v1 list ranked by TAP COUNT and every entry proved irrelevant
// (tone/grade pass, glyph filter, material ubershaders). That ranking was the
// mistake: AA is confirmed present on alpha-tested cutouts as well as
// geometry with MSAA off, which no MSAA-like mechanism can do - it needs a
// whole-image operation, and a temporal/accumulation resolve is only
// 2 taps (current + history) so tap ranking actively hid it.
//
// These are the shaders RUNTIME attribution proved are bound for fullscreen
// quads, ordered by suspicion:
//   #1 ps_5CFEC5A6 - 2xtexld, mul, LRP. An lrp between two textures is
//      literally lerp(current, history, k): the shape of a temporal resolve.
//   #2 ps_1983C6B6 / #3 ps_EBAA9CE9 - 2-tap, appear in both MULTI_SAMPLE and
//      DRAW_FILTER, the other blend-shaped candidates.
//   #4 ps_68A227BC - 2-tap with the largest draw count of any post shader.
//   #5-8 - the cmp-heavy 1-tap filters from DRAW_FILTER.
static const DWORD g_psKillCandidates[] = {
    0x5CFEC5A6u,   // 2 taps: texld x2, mul, LRP  <- temporal-resolve shape
    0x1983C6B6u,   // 2 taps, MULTI_SAMPLE + DRAW_FILTER
    0xEBAA9CE9u,   // 2 taps, mad-heavy
    0x68A227BCu,   // 2 taps, ~600k draws
    0x0619441Cu,   // 1 tap, 25 instrs, 11x cmp
    0x97308FE6u,   // 1 tap, 54 instrs, 8x cmp
    0xD1BDD522u,   // 1 tap, 12 instrs
    0x77C4B5D4u,   // 2 taps, MULTI_SAMPLE
    0x20656E5Fu,   // 2 taps, dp3-heavy
    0xE5537D53u,   // 1 tap, MULTI_SAMPLE
    0x23A804BFu,   // 1 tap, simplest blit
    0xDCD57A17u,   // 25 taps - the GLYPH OUTLINE filter (HD GUI mod lever)
};
// (Graphics_Scaling has no GUI control here - it is an in-game menu option.)
// A2C candidate identify: hot dropdown, because walking candidates by editing
// the ini and relaunching is not a workflow anyone should be asked to use.
#define IDC_A2CID_COMBO 3016
// A long STATIC list beats a short reordering one: entries are keyed by
// shader creation index and never move, so it can be stepped through fast.
#define PS_IDENTIFY_LIST_MAX 16000

#define NUM_PS_KILL (sizeof(g_psKillCandidates)/sizeof(g_psKillCandidates[0]))
static const char *g_psKillLabels[] = {
    "Off (kill nothing)",
    "1: ps_5CFEC5A6  LRP blend <- prime AA suspect",
    "2: ps_1983C6B6  2-tap blend",
    "3: ps_EBAA9CE9  2-tap",
    "4: ps_68A227BC  2-tap, most draws",
    "5: ps_0619441C  1-tap, cmp-heavy",
    "6: ps_97308FE6  1-tap, cmp-heavy",
    "7: ps_D1BDD522  1-tap",
    "8: ps_77C4B5D4  2-tap",
    "9: ps_20656E5F  2-tap dp3",
    "10: ps_E5537D53  1-tap",
    "11: ps_23A804BF  1-tap blit",
    "12: ps_DCD57A17  glyph outlines (HD GUI)",
};

static HWND g_hCheckBoxes[NUM_TOGGLES];
static HWND g_hStatsLabel = NULL;
static HWND g_hThreshEdit = NULL;
static HWND g_hFpsEdit = NULL;
static HWND g_hDeferEdit = NULL;
static HWND g_hShadowCombo = NULL;
static HWND g_hShadowBufCombo = NULL;
static HWND g_hSplitNearCombo = NULL;
static HWND g_hSplitFarCombo = NULL;
static HWND g_hTalkCombo = NULL;
static HWND g_hMsaaCombo = NULL;
static HWND g_hA2cCheck = NULL;   // retired from the panel; see the texkill note
static HWND g_hFxaaCombo = NULL;
static HWND g_hA2cIdCombo = NULL;
static HWND g_hA2cIdLabel = NULL;

// ---- Frametime graph overlay ---------------------------------------------
// A layered, click-through, always-on-top window drawn over the game with
// plain GDI - deliberately NOT a D3D9 overlay rendered through the game's own
// pipeline. Same reasoning as the control panel: this whole investigation has
// been chasing bugs in that pipeline, an in-engine overlay would need the
// Present hook (which has never once fired on this game's real device), and
// an instrument that perturbs the thing it measures is worse than useless
// here. A separate window costs the render path nothing.
//
// It graphs the ENGINE TICK, which is the entire point: RTSS graphs presents,
// and this game's presentation is decoupled from its logic tick, so RTSS
// shows a flat line through hitches that are plainly visible. This shows what
// the game logic actually did.
//
// Works because the game runs Windowed (confirmed: `Windowed=1` on the real
// swap chain). A true exclusive-fullscreen device would not be overlayable
// this way.
// Sized for a 4K capture that gets viewed at 1080p: everything is halved on
// the way down, so the text is drawn at roughly 2x what would be comfortable
// natively. GDI's default font is ~16px and becomes illegible after that
// downscale, hence an explicit font rather than the stock one.
#define OVL_W 1000
#define OVL_H 320
#define OVL_FONT_H 34            // header/footer text height in pixels
#define OVL_LABEL_H 24           // reference-line labels
// Plot ceiling and colour bands are all derived from g_stutterThresholdUsec,
// so changing the threshold rescales the graph instead of leaving bars
// clipped or the whole thing amber. 2.2x keeps the "2x threshold" line
// comfortably inside the plot.
static HWND g_hOverlay = NULL;
static HFONT g_ovlFont = NULL, g_ovlFontSmall = NULL;
// Default bottom-RIGHT. The game's menu bar runs along the top and this window
// is topmost, so the top corners cover it; bottom-right is furthest from both
// the menu and the game's own bottom-left HUD elements.
static volatile LONG g_overlayPos = 3;

static void EnsureOverlayFonts(void)
{
    if (!g_ovlFont) {
        // Negative height = character height rather than cell height. Consolas
        // is fixed-width, so the numbers stop jittering horizontally as they
        // change - which matters a lot when reading a live readout.
        g_ovlFont = CreateFontA(-OVL_FONT_H, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    }
    if (!g_ovlFontSmall) {
        g_ovlFontSmall = CreateFontA(-OVL_LABEL_H, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    }
}

static void DrawOverlayGraph(HDC dc)
{
    EnsureOverlayFonts();
    HGDIOBJ oldFont = SelectObject(dc, g_ovlFont);
    RECT full = { 0, 0, OVL_W, OVL_H };
    HBRUSH bg = CreateSolidBrush(RGB(12, 12, 16));
    FillRect(dc, &full, bg);
    DeleteObject(bg);

    const int padL = 10, padT = OVL_FONT_H + 14, padB = OVL_FONT_H + 12;
    int plotH = OVL_H - padT - padB;
    int plotW = OVL_W - padL * 2;

    // Reference lines: 60fps (16.7ms) and 30fps (33.3ms). Anything touching
    // the 16.7 line is a frame that failed to sustain 60fps.
    // The graph's scale is derived from the threshold, but the threshold now
    // defaults to 1s (watchdog effectively off) - which would scale the plot
    // to 2.2 SECONDS and flatten every real frame into the bottom pixel.
    // Clamp the scaling input so the two settings stop being coupled: the
    // watchdog can be as insensitive as it likes while the graph stays
    // readable around the 16.7/33.3ms reference lines.
    LONG thr = g_stutterThresholdUsec;
    if (thr < 1000) thr = 1000;
    if (thr > 33333) thr = 33333;
    LONG plotMax = (LONG)(thr * 2.2);
    char lbl1[16], lbl2[16];
    sprintf(lbl1, "%.1f", thr / 1000.0);
    sprintf(lbl2, "%.1f", thr * 2 / 1000.0);
    struct { LONG us; COLORREF c; const char *label; } refs[2] = {
        { thr,     RGB(70, 120, 70), lbl1 },
        { thr * 2, RGB(120, 70, 70), lbl2 },
    };
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < 2; i++) {
        int y = padT + plotH - (int)((double)refs[i].us / plotMax * plotH);
        if (y < padT || y > padT + plotH) continue;
        HPEN pen = CreatePen(PS_SOLID, 2, refs[i].c);
        HPEN old = (HPEN)SelectObject(dc, pen);
        MoveToEx(dc, padL, y, NULL);
        LineTo(dc, padL + plotW, y);
        SelectObject(dc, old);
        DeleteObject(pen);
        SetTextColor(dc, refs[i].c);
        SelectObject(dc, g_ovlFontSmall);
        TextOutA(dc, padL + plotW - 64, y - OVL_LABEL_H - 2, refs[i].label, (int)strlen(refs[i].label));
    }

    // Bars, oldest to newest left-to-right. Colour encodes severity so a
    // glance is enough: green under 60fps-equivalent, amber past it, red past
    // 30fps-equivalent.
    LONG pos = g_frameRingPos;
    LONG worst = 0;
    for (int i = 0; i < FRAME_RING && i < plotW; i++) {
        LONG idx = (pos - FRAME_RING + i) % FRAME_RING;
        if (idx < 0) idx += FRAME_RING;
        LONG us = g_frameRing[idx];
        if (us <= 0) continue;
        if (us > worst) worst = us;
        int h = (int)((double)us / plotMax * plotH);
        if (h > plotH) h = plotH;
        if (h < 1) h = 1;
        COLORREF c = (us > thr * 2) ? RGB(230, 70, 70)
                   : (us > thr)     ? RGB(230, 180, 60)
                                    : RGB(90, 200, 110);
        int x = padL + (plotW * i) / FRAME_RING;
        RECT bar = { x, padT + plotH - h, x + 3, padT + plotH };
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &bar, b);
        DeleteObject(b);
    }

    // Kept short on purpose: at ~34px Consolas roughly 50 characters fit
    // across OVL_W, and an overflowing header is worse than a terse one.
    // The "which clock is this" caption lives in the footer, which has room.
    char hdr[160];
    LONG p50 = g_liveP50;
    sprintf(hdr, "%.0ffps  p50 %.1f  p99 %.1f  max %.1f  >%.1f %ld/%ld",
            p50 > 0 ? 1000000.0 / (double)p50 : 0.0,
            p50 / 1000.0, g_liveP99 / 1000.0, g_liveWorst / 1000.0,
            thr / 1000.0, g_liveOver16, g_liveFrames);
    SetTextColor(dc, RGB(235, 235, 245));
    SelectObject(dc, g_ovlFont);
    TextOutA(dc, padL, 6, hdr, (int)strlen(hdr));

    char foot[96];
    sprintf(foot, "ENGINE TICK - game logic, not presents (%d)", FRAME_RING);
    SetTextColor(dc, RGB(150, 150, 165));
    SelectObject(dc, g_ovlFont);
    TextOutA(dc, padL, OVL_H - OVL_FONT_H - 8, foot, (int)strlen(foot));
    SelectObject(dc, oldFont);
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        // Double-buffered: painting bar-by-bar straight to the window
        // flickers badly at this repaint rate.
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, OVL_W, OVL_H);
        HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);
        DrawOverlayGraph(mem);
        BitBlt(dc, 0, 0, OVL_W, OVL_H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    // Draggable from anywhere, same as the status panel - the four-corner
    // menu it replaces could not put the graph where the HUD wasn't.
    if (msg == WM_NCHITTEST) return HTCAPTION;
    if (msg == WM_EXITSIZEMOVE || msg == WM_NCLBUTTONUP) {
        RECT rc;
        if (GetWindowRect(hwnd, &rc)) {
            if (rc.left != g_overlayX || rc.top != g_overlayY) {
                InterlockedExchange(&g_overlayX, rc.left);
                InterlockedExchange(&g_overlayY, rc.top);
                SaveConfig();
            }
        }
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;   // fully repainted above
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void EnsureOverlayWindow(void)
{
    if (g_hOverlay) return;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "LRStutterOverlay";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);
    // WS_EX_TRANSPARENT is deliberately NOT set: click-through and draggable
    // are mutually exclusive, and being able to place it beats never catching
    // a click. WS_EX_NOACTIVATE still keeps it from taking focus.
    g_hOverlay = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "LRStutterOverlay", "", WS_POPUP,
        20, 20, OVL_W, OVL_H, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (g_hOverlay) SetLayeredWindowAttributes(g_hOverlay, 0, 205, LWA_ALPHA);
}

// ---- Status panel --------------------------------------------------------
// Answers "is the mod doing what I set?" - every setting beside the value
// ACTUALLY in force this frame, which is not always the same thing: the
// cutscene logic overrides the shadow distance, SSAA only supersamples when
// the presentation is clamped, MSAA needs the identity latch to bite. Reading
// that out of a 3MB log after the fact was the slow part of every test run.
//
// Deliberately a SECOND window rather than a mode on the frametime graph:
// they answer different questions and are useful simultaneously.
#define STAT_FONT_H 13           // small + dense: this panel is read, not glanced
#define STAT_ROW_H  17
#define STAT_W      470
#define STAT_H      228           // 10 rows + header
#define STAT_COL_L  12           // label
#define STAT_COL_S  170          // configured value
#define STAT_COL_A  310          // value actually in force
static HWND  g_hStatus = NULL;
static HFONT g_statFont = NULL, g_statFontB = NULL;

static void EnsureStatusFonts(void)
{
    // Its own fonts, NOT the graph's. The graph is meant to be readable from
    // across the room at 34px bold; reusing that here is what made the first
    // version overflow its own window and collide column with column.
    if (!g_statFont)
        g_statFont = CreateFontA(-STAT_FONT_H, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    if (!g_statFontB)
        g_statFontB = CreateFontA(-STAT_FONT_H, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
}

// One row: label, configured value, value actually applied. `differs` paints
// the applied column amber - the entire reason the panel exists.
static void StatRow(HDC dc, int *y, const char *label, const char *set,
                    const char *applied, int differs)
{
    SetTextColor(dc, RGB(145, 145, 160));
    TextOutA(dc, STAT_COL_L, *y, label, (int)strlen(label));
    SetTextColor(dc, RGB(195, 195, 210));
    TextOutA(dc, STAT_COL_S, *y, set, (int)strlen(set));
    SetTextColor(dc, differs ? RGB(240, 185, 70) : RGB(120, 205, 140));
    TextOutA(dc, STAT_COL_A, *y, applied, (int)strlen(applied));
    *y += STAT_ROW_H;
}

static void DrawStatusPanel(HDC dc)
{
    char set[64], app[64];
    RECT full = { 0, 0, STAT_W, STAT_H };
    HBRUSH bg = CreateSolidBrush(RGB(14, 14, 18));
    int y = 9;
    EnsureStatusFonts();
    FillRect(dc, &full, bg);
    DeleteObject(bg);
    // Without this every string paints its own opaque black box, which is
    // what gave the first version its blocky, chopped-up look.
    SetBkMode(dc, TRANSPARENT);

    SelectObject(dc, g_statFontB);
    SetTextColor(dc, RGB(235, 235, 245));
    TextOutA(dc, STAT_COL_L, y, "MOD STATUS", 10);
    SetTextColor(dc, RGB(105, 105, 120));
    TextOutA(dc, STAT_COL_S, y, "SETTING", 7);
    TextOutA(dc, STAT_COL_A, y, "APPLIED NOW", 11);
    y += STAT_ROW_H + 6;
    SelectObject(dc, g_statFont);

    // --- the live state this panel was built for --------------------------
    {
        int cut = (int)g_cutsceneActive;
        sprintf(set, "%s", g_cutsceneRevert ? "auto-revert on" : "auto-revert off");
        // Show WHICH cut is holding it - if it ever triggers on something it
        // should not, the name is the answer, and it is right here.
        if (cut && g_cutsceneName[0]) sprintf(app, "CUTSCENE %.14s", g_cutsceneName);
        else if (cut)                 sprintf(app, "CUTSCENE");
        else                          sprintf(app, "gameplay");
        StatRow(dc, &y, "Scene state", set, app, cut);
    }
    {
        LONG n = g_shadowSplitNearPct;
        int held = (g_cutsceneActive && n > 0);
        if (n > 0) sprintf(set, "%ld%%", n); else sprintf(set, "off");
        if (held) sprintf(app, "100%% (cutscene)");
        else if (n > 0) sprintf(app, "%ld%%", n);
        else sprintf(app, "off");
        StatRow(dc, &y, "Shadow distance", set, app, held);
    }
    {
        LONG f = g_shadowSplitFarPct;
        int held = (g_cutsceneActive && f > 0);
        if (f > 0) sprintf(set, "%ld%%", f); else sprintf(set, "off");
        if (held) sprintf(app, "100%% (cutscene)");
        else if (f > 0) sprintf(app, "%ld%%", f);
        else sprintf(app, "off");
        StatRow(dc, &y, "Shadow far split", set, app, held);
    }
    y += 7;
    // --- the rest of the graphics state -----------------------------------
    {
        LONG r = g_shadowMapRes;
        if (r > 0) sprintf(set, "%ld", r); else sprintf(set, "game default");
        sprintf(app, "%s", g_shadowResWrites > 0 ? "written" : "not written");
        StatRow(dc, &y, "Shadow map res", set, app, r > 0 && g_shadowResWrites == 0);
    }
    {
        LONG p = g_shadowBufResPct;
        if (p > 0) { sprintf(set, "%ld%%", p); sprintf(app, "%ld%%", p); }
        else       { sprintf(set, "game default"); sprintf(app, "-"); }
        StatRow(dc, &y, "Shadow buffer", set, app, 0);
    }
    {
        LONG sc = g_ssaaScale;
        int on = (g_ssaaActive != 0);
        if (sc == 100) sprintf(set, "off"); else sprintf(set, "%ld%%", sc);
        if (on && g_ssaaCurW > 0) sprintf(app, "%ldx%ld", g_ssaaCurW, g_ssaaCurH);
        else if (on)              sprintf(app, "active");
        else                      sprintf(app, "off");
        StatRow(dc, &y, "SSAA", set, app, (sc != 100) != on);
    }
    {
        LONG m = g_msaaSamples;
        if (m > 0) sprintf(set, "%ldx", m); else sprintf(set, "off");
        sprintf(app, "%ld subs", g_msSubstitutions);
        StatRow(dc, &y, "MSAA", set, app, m > 0 && g_msSubstitutions == 0);
    }
    {
        // The engine's built-in FXAA. g_fxaaOff=1 swaps the pass for a
        // passthrough shader, so "removed" is only true once that shader has
        // actually been substituted at least once.
        LONG off = g_fxaaOff;
        sprintf(set, "%s", off ? "removed" : "vanilla (on)");
        sprintf(app, "%s", off ? (g_fxaaSubs > 0 ? "passthrough" : "not hit yet")
                               : "active");
        StatRow(dc, &y, "Built-in FXAA", set, app, off && g_fxaaSubs == 0);
    }
    // REMOVED: "Output" (resolution + format) and "Display mode"
    // (fullscreen/windowed). Both read straight from the present parameters
    // and both were wrong on screen - at a 1080p fullscreen setting on a 4K
    // display they reported 3840x2160 and "windowed".
    //
    // That is not a bug in the readout, it is the same decoupling the SSAA
    // work already documented: the engine presents into a desktop-sized
    // backbuffer and lets the display clamp, and it uses a borderless window
    // rather than exclusive fullscreen, so D3D's own numbers genuinely say
    // 4K/windowed while the player is looking at 1080p fullscreen. Reporting
    // the resolution a player would recognise means deriving it from the
    // engine's internal size instead, which is a different job. Not worth it
    // for a status row - removed rather than left showing numbers that
    // disagree with the game's own menu.
    {
        LONG c = g_targetFpsX100;
        if (c > 0) sprintf(set, "%.2f fps", c / 100.0); else sprintf(set, "unlocked");
        sprintf(app, "p50 %.1f ms", g_liveP50 / 1000.0);
        StatRow(dc, &y, "Frame cap", set, app, 0);
    }
    {
        LONG t = g_stutterThresholdUsec;
        if (t >= 500000) sprintf(set, "off"); else sprintf(set, "%ld ms", t / 1000);
        sprintf(app, "%ld over", g_liveOver16);
        StatRow(dc, &y, "Stutter watchdog", set, app, 0);
    }

}

static LRESULT CALLBACK StatusWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, STAT_W, STAT_H);
        HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);
        DrawStatusPanel(mem);
        BitBlt(dc, 0, 0, STAT_W, STAT_H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    // Drag from anywhere on the panel: it has no title bar to grab, and one
    // is not worth the vertical space. HTCAPTION makes the whole surface the
    // caption as far as the window manager is concerned.
    if (msg == WM_NCHITTEST) return HTCAPTION;
    // Remember where it was dropped, and persist it. Done on button-up rather
    // than per WM_MOVE so a drag writes the ini once instead of every pixel.
    if (msg == WM_EXITSIZEMOVE || msg == WM_NCLBUTTONUP) {
        RECT rc;
        if (GetWindowRect(hwnd, &rc)) {
            if (rc.left != g_statusX || rc.top != g_statusY) {
                InterlockedExchange(&g_statusX, rc.left);
                InterlockedExchange(&g_statusY, rc.top);
                SaveConfig();
            }
        }
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void EnsureStatusWindow(void)
{
    if (g_hStatus) return;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = StatusWndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "LRStutterStatus";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);
    // NO WS_EX_TRANSPARENT here, unlike the frametime graph: this one has to
    // receive the mouse to be draggable. WS_EX_NOACTIVATE still keeps it from
    // taking focus off the game when it is clicked.
    g_hStatus = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "LRStutterStatus", "", WS_POPUP,
        20, 20, STAT_W, STAT_H, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (g_hStatus) SetLayeredWindowAttributes(g_hStatus, 0, 225, LWA_ALPHA);
}

// Auto-place in the corner OPPOSITE the frametime graph so the two cannot
// overlap - but only until the user drags it somewhere, after which their
// position wins and this does nothing.
static void PositionStatusPanel(void)
{
    RECT rc;
    POINT tl, br;
    HWND gw = g_gameHwnd;
    int have = 0, x = 20, y = 20;
    const int m = 20;
    if (g_statusX >= 0 && g_statusY >= 0) {
        SetWindowPos(g_hStatus, HWND_TOPMOST, g_statusX, g_statusY, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    if (gw && IsWindow(gw) && GetClientRect(gw, &rc) &&
        rc.right > rc.left && rc.bottom > rc.top) {
        tl.x = rc.left;  tl.y = rc.top;
        br.x = rc.right; br.y = rc.bottom;
        if (ClientToScreen(gw, &tl) && ClientToScreen(gw, &br)) have = 1;
    }
    if (!have) {
        RECT wa;
        if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
            tl.x = wa.left;  tl.y = wa.top;
            br.x = wa.right; br.y = wa.bottom;
            have = 1;
        }
    }
    // Top-right; the graph auto-places bottom-left, so a fresh install shows
    // both without them overlapping. Either can then be dragged anywhere.
    if (have) { x = br.x - STAT_W - m; y = tl.y + m; }
    SetWindowPos(g_hStatus, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

// Overlay driver thread. The overlay used to be driven from the control
// panel's 250ms sync timer, which made a shipping feature depend on a
// debug window; with the panel deprecated (ENABLE_GUI_PANEL 0) it owns its
// own thread. Sole owner either way - the panel's timer no longer touches
// the overlay, so there is no double-drive when the panel is compiled back in.
// It now drives the status panel as well, on the same 250ms tick.
//
// PeekMessage pump rather than SetTimer: the overlay window is created on
// THIS thread, so this loop is what dispatches its WM_PAINT. Same 250ms
// cadence as the old timer.
// ---- SSAO tuning window (v25, user request: live tweaking) ---------------
// A small captioned tool window with one scrollbar per SSAO tunable. Values
// take effect the NEXT FRAME (the shader reads the volatile LONGs every
// draw), so dragging a slider tunes the effect live. SCROLLBAR controls
// rather than comctl32 trackbars: user32-only, nothing new linked.
// Opened/closed from Dev Tools > SSAO Tuning Panel; the window's own close
// button clears the same flag, so the checkbox stays honest.
#if ENABLE_AO_SSAO
static HWND g_hAoTweak = NULL;

// val is the ACTIVE binding; vals[] holds the per-estimator slots
// ([0]=SSAO [1]=HBAO). The overlay thread re-points val when the estimator
// changes, so the panel always edits the live estimator's values. Shared
// rows (Projection, Blur Sharp) list the same pointer twice.
static struct {
    const char *name;
    volatile LONG *val;
    volatile LONG *vals[2];
    LONG lo, hi, step;
    HWND bar;
} g_aoRows[] = {
    { "Strength %",  &g_aoStrengthPctE[0], { &g_aoStrengthPctE[0], &g_aoStrengthPctE[1] },  0,  200,  5, NULL },
    { "Intensity",   &g_aoIntensityE[0],   { &g_aoIntensityE[0],   &g_aoIntensityE[1] },   50, 2000, 25, NULL },
    { "Radius",      &g_aoRadiusE[0],      { &g_aoRadiusE[0],      &g_aoRadiusE[1] },      10, 1500, 10, NULL },
    { "Projection",  &g_aoProj100,         { &g_aoProj100,         &g_aoProj100 },         80,  250,  5, NULL },
    // Blur depth edge-stop (0 = plain gaussian). Only meaningful with
    // AoBlur=1; the row is harmless when the blur is off.
    { "Blur Sharp",  &g_aoBlurSharp,       { &g_aoBlurSharp,       &g_aoBlurSharp },        0,  400, 10, NULL },
};
#define AO_ROWS (sizeof(g_aoRows) / sizeof(g_aoRows[0]))
#define AOTW_ROW_H   34
#define AOTW_LABEL_W 120
#define AOTW_BAR_W   230
#define AOTW_VAL_W   50

static LRESULT CALLBACK AoTweakProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_HSCROLL: {
        HWND bar = (HWND)lp;
        for (size_t i = 0; i < AO_ROWS; i++) {
            if (g_aoRows[i].bar != bar) continue;
            LONG v = *g_aoRows[i].val;
            int code = LOWORD(wp);
            if (code == SB_THUMBTRACK || code == SB_THUMBPOSITION) v = (LONG)HIWORD(wp);
            else if (code == SB_LINELEFT)  v -= g_aoRows[i].step;
            else if (code == SB_LINERIGHT) v += g_aoRows[i].step;
            else if (code == SB_PAGELEFT)  v -= g_aoRows[i].step * 4;
            else if (code == SB_PAGERIGHT) v += g_aoRows[i].step * 4;
            else if (code == SB_ENDSCROLL) { SaveConfig(); return 0; }
            if (v < g_aoRows[i].lo) v = g_aoRows[i].lo;
            if (v > g_aoRows[i].hi) v = g_aoRows[i].hi;
            InterlockedExchange(g_aoRows[i].val, v);
            SetScrollPos(bar, SB_CTL, (int)v, TRUE);
            InvalidateRect(h, NULL, TRUE);
            return 0;
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT old = (HFONT)SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        char t[64];
        for (size_t i = 0; i < AO_ROWS; i++) {
            int y = 10 + (int)i * AOTW_ROW_H;
            TextOutA(dc, 10, y + 4, g_aoRows[i].name, (int)strlen(g_aoRows[i].name));
            sprintf(t, "%ld", *g_aoRows[i].val);
            TextOutA(dc, 10 + AOTW_LABEL_W + AOTW_BAR_W + 8, y + 4, t, (int)strlen(t));
        }
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        // Hide, don't destroy: reopening keeps positions, and the menu
        // checkbox mirrors this flag so it unticks itself.
        InterlockedExchange(&g_aoTweakOpen, 0);
        ShowWindow(h, SW_HIDE);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static void EnsureAoTweakWindow(void)
{
    if (g_hAoTweak) return;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = AoTweakProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "LRSaviorAoTweak";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);
    int cw = 10 + AOTW_LABEL_W + AOTW_BAR_W + 8 + AOTW_VAL_W + 10;
    int ch = 20 + (int)AO_ROWS * AOTW_ROW_H;
    RECT r = { 0, 0, cw, ch };
    AdjustWindowRectEx(&r, WS_CAPTION | WS_SYSMENU | WS_POPUP, FALSE, WS_EX_TOOLWINDOW);
    g_hAoTweak = CreateWindowExA(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName,
        (g_aoEnable == 2) ? "AO Tuning - HBAO" : "AO Tuning - SSAO",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        120, 120, r.right - r.left, r.bottom - r.top,
        NULL, NULL, wc.hInstance, NULL);
    if (!g_hAoTweak) return;
    for (size_t i = 0; i < AO_ROWS; i++) {
        int y = 10 + (int)i * AOTW_ROW_H;
        g_aoRows[i].bar = CreateWindowExA(
            0, "SCROLLBAR", NULL, WS_CHILD | WS_VISIBLE | SBS_HORZ,
            10 + AOTW_LABEL_W, y, AOTW_BAR_W, 18,
            g_hAoTweak, NULL, wc.hInstance, NULL);
        SCROLLINFO si;
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_POS | SIF_PAGE;
        si.nMin = (int)g_aoRows[i].lo;
        si.nMax = (int)g_aoRows[i].hi;
        si.nPos = (int)*g_aoRows[i].val;
        si.nPage = 1;
        SetScrollInfo(g_aoRows[i].bar, SB_CTL, &si, TRUE);
    }
}
#endif // ENABLE_AO_SSAO

static DWORD WINAPI OverlayThread(LPVOID param)
{
    (void)param;
    LogLine("[boot] overlay: driver thread entered");
    for (;;) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (g_overlayEnabled) {
            EnsureOverlayWindow();
            if (g_hOverlay) {
                if (!IsWindowVisible(g_hOverlay)) ShowWindow(g_hOverlay, SW_SHOWNOACTIVATE);
                // Dragged position wins; otherwise auto-place bottom-left of
                // the GAME's client area (not the desktop, so windowed mode
                // works too). Re-asserts topmost each tick: the game
                // reasserts its own z-order on focus changes and would
                // otherwise cover this.
                if (g_overlayX >= 0 && g_overlayY >= 0) {
                    SetWindowPos(g_hOverlay, HWND_TOPMOST, g_overlayX, g_overlayY, 0, 0,
                                 SWP_NOSIZE | SWP_NOACTIVATE);
                } else {
                    int x = 20, y = 20;
                    const int m = 20;
                    RECT rc;
                    HWND gw = g_gameHwnd;
                    POINT tl, br;
                    int have = 0;
                    if (gw && IsWindow(gw) && GetClientRect(gw, &rc) &&
                        rc.right > rc.left && rc.bottom > rc.top) {
                        tl.x = rc.left;  tl.y = rc.top;
                        br.x = rc.right; br.y = rc.bottom;
                        if (ClientToScreen(gw, &tl) && ClientToScreen(gw, &br)) have = 1;
                    }
                    if (!have) {           // pre-Reset, or the window is gone
                        RECT wa;
                        if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
                            tl.x = wa.left;  tl.y = wa.top;
                            br.x = wa.right; br.y = wa.bottom;
                            have = 1;
                        }
                    }
                    // Bottom-left: the status panel auto-places top-right, so
                    // the two never land on each other before either is moved.
                    if (have) { x = tl.x + m; y = br.y - OVL_H - m; }
                    SetWindowPos(g_hOverlay, HWND_TOPMOST, x, y, 0, 0,
                                 SWP_NOSIZE | SWP_NOACTIVATE);
                }
                InvalidateRect(g_hOverlay, NULL, FALSE);
            }
        } else if (g_hOverlay && IsWindowVisible(g_hOverlay)) {
            ShowWindow(g_hOverlay, SW_HIDE);
        }
#if ENABLE_AO_SSAO
        {
            // Per-estimator slider retarget: whenever the AO menu switches
            // estimator, re-point every row at the live slot, snap the
            // scrollbars to the slot's values, and retitle the window so
            // the panel says which estimator it is editing.
            static LONG lastEst = -1;
            LONG est = (g_aoEnable == 2) ? 1 : 0;
            if (est != lastEst) {
                lastEst = est;
                for (size_t i = 0; i < AO_ROWS; i++) {
                    g_aoRows[i].val = g_aoRows[i].vals[est];
                    if (g_aoRows[i].bar)
                        SetScrollPos(g_aoRows[i].bar, SB_CTL, (int)*g_aoRows[i].val, TRUE);
                }
                if (g_hAoTweak) {
                    SetWindowTextA(g_hAoTweak, est ? "AO Tuning - HBAO" : "AO Tuning - SSAO");
                    InvalidateRect(g_hAoTweak, NULL, TRUE);
                }
            }
        }
        if (g_aoTweakOpen) {
            EnsureAoTweakWindow();
            if (g_hAoTweak && !IsWindowVisible(g_hAoTweak)) {
                // Refresh scrollbar positions from the (possibly ini-edited)
                // values before showing, then activate: unlike the overlay,
                // this window WANTS focus - it is a control surface.
                for (size_t i = 0; i < AO_ROWS; i++)
                    if (g_aoRows[i].bar)
                        SetScrollPos(g_aoRows[i].bar, SB_CTL, (int)*g_aoRows[i].val, TRUE);
                ShowWindow(g_hAoTweak, SW_SHOW);
            }
        } else if (g_hAoTweak && IsWindowVisible(g_hAoTweak)) {
            ShowWindow(g_hAoTweak, SW_HIDE);
        }
#endif
        if (g_statusEnabled) {
            EnsureStatusWindow();
            if (g_hStatus) {
                if (!IsWindowVisible(g_hStatus)) ShowWindow(g_hStatus, SW_SHOWNOACTIVATE);
                PositionStatusPanel();
                InvalidateRect(g_hStatus, NULL, FALSE);
            }
        } else if (g_hStatus && IsWindowVisible(g_hStatus)) {
            ShowWindow(g_hStatus, SW_HIDE);
        }
        Sleep(250);
    }
}

// ---- DEPRECATED: the standalone control-panel window ----------------------
// Superseded 2026-08-11 by the game-menu integration above (ENABLE_GAME_MENU).
// From now on new options go in the game's own menu bar; this window is not
// the place to add them. Gated rather than deleted, per the project rule -
// it was the mod's only UI for most of this investigation and it still
// documents every control that ever existed, including the diagnostic
// dropdowns that live behind ENABLE_SHADER_DIAG.
//
// Note the one real coupling that had to be broken to retire it: the
// frametime overlay was driven by this window's sync timer. That drive loop
// now lives in OverlayThread above, so the overlay is unaffected.
#if ENABLE_GUI_PANEL

static LRESULT CALLBACK PanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        for (size_t i = 0; i < NUM_TOGGLES; i++) {
            g_hCheckBoxes[i] = CreateWindowA("BUTTON", g_toggles[i].label,
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                10, 10 + (int)i * 26, 300, 24, hwnd,
                (HMENU)(UINT_PTR)(IDC_CHECK_BASE + i), NULL, NULL);
        }
        // Live frametime readout. This exists because RTSS measures
        // PRESENTS, and this game's presentation runs on a cadence
        // independent of the logic tick - its graph stays flat through a
        // visible hitch, which makes it untrustworthy here. These numbers
        // come from the engine's own per-tick timing, so they show what the
        // game is actually doing.
        {
            int y = 10 + (int)NUM_TOGGLES * 26 + 6;
            CreateWindowA("STATIC", "Stutter/graph threshold (ms):", WS_CHILD | WS_VISIBLE,
                10, y + 4, 180, 20, hwnd, NULL, NULL, NULL);
            char cur[32];
            sprintf(cur, "%.1f", g_stutterThresholdUsec / 1000.0);
            g_hThreshEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", cur,
                WS_CHILD | WS_VISIBLE | ES_LEFT,
                195, y, 55, 24, hwnd, (HMENU)(UINT_PTR)IDC_THRESH_EDIT, NULL, NULL);
            CreateWindowA("BUTTON", "Apply", WS_CHILD | WS_VISIBLE,
                258, y, 60, 24, hwnd, (HMENU)(UINT_PTR)IDC_THRESH_APPLY, NULL, NULL);

            // Target FPS lock. Only meaningful while UnlockFramerate is also
            // checked - see ApplyFramerateUnlock for the known CPU-cost
            // tradeoff (this reuses the game's own Sleep-spin limiter, so it
            // is NOT free like the plain unlock) and the open question of
            // whether it helps the frame-to-frame jitter symptom at all.
            int y2 = y + 30;
            CreateWindowA("STATIC", "Target FPS (0=unlocked, needs Unlock on):", WS_CHILD | WS_VISIBLE,
                10, y2 + 4, 260, 20, hwnd, NULL, NULL, NULL);
            char curFps[32];
            sprintf(curFps, "%.2f", g_targetFpsX100 / 100.0);
            g_hFpsEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", curFps,
                WS_CHILD | WS_VISIBLE | ES_LEFT,
                275, y2, 60, 24, hwnd, (HMENU)(UINT_PTR)IDC_FPS_EDIT, NULL, NULL);
            CreateWindowA("BUTTON", "Apply", WS_CHILD | WS_VISIBLE,
                340, y2, 60, 24, hwnd, (HMENU)(UINT_PTR)IDC_FPS_APPLY, NULL, NULL);

#if ENABLE_DEFER_UPLOADS
            // Deferred-upload rate. Only meaningful while DeferUploads is
            // also checked - see SubmitStagedUpload/DrainStagedUploads.
            int y3 = y2 + 30;
            CreateWindowA("STATIC", "Deferred uploads per frame (needs Defer on):", WS_CHILD | WS_VISIBLE,
                10, y3 + 4, 260, 20, hwnd, NULL, NULL, NULL);
            char curDefer[32];
            sprintf(curDefer, "%ld", g_deferPerFrame);
            g_hDeferEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", curDefer,
                WS_CHILD | WS_VISIBLE | ES_LEFT,
                275, y3, 60, 24, hwnd, (HMENU)(UINT_PTR)IDC_DEFER_EDIT, NULL, NULL);
            CreateWindowA("BUTTON", "Apply", WS_CHILD | WS_VISIBLE,
                340, y3, 60, 24, hwnd, (HMENU)(UINT_PTR)IDC_DEFER_APPLY, NULL, NULL);
#else
            int y3 = y2;   // defer control retired; rows below move up
#endif

            // Shadow map resolution. A dropdown rather than an edit box
            // because the engine only ever uses discrete power-of-two sizes
            // and a typo here writes into its live settings object.
            int y4 = y3 + 30;
            CreateWindowA("STATIC", "Shadow map resolution:", WS_CHILD | WS_VISIBLE,
                10, y4 + 4, 150, 20, hwnd, NULL, NULL, NULL);
            // Height here budgets for the dropped-down list, not the closed box.
            g_hShadowCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                165, y4, 235, 200, hwnd, (HMENU)(UINT_PTR)IDC_SHADOW_COMBO, NULL, NULL);
            {
                int sel = 0;
                for (size_t i = 0; i < NUM_SHADOW_CHOICES; i++) {
                    SendMessageA(g_hShadowCombo, CB_ADDSTRING, 0, (LPARAM)g_shadowResLabels[i]);
                    if (g_shadowResChoices[i] == g_shadowMapRes) sel = (int)i;
                }
                SendMessageA(g_hShadowCombo, CB_SETCURSEL, sel, 0);
            }

            // Screen-space shadow buffer resolution. Same creation-time
            // caveat as the shadow map above: it applies when the buffers are
            // next built, i.e. on an area change or restart.
            int y4b = y4 + 30;
            CreateWindowA("STATIC", "Shadow buffer res:", WS_CHILD | WS_VISIBLE,
                10, y4b + 4, 150, 20, hwnd, NULL, NULL, NULL);
            g_hShadowBufCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                165, y4b, 235, 200, hwnd, (HMENU)(UINT_PTR)IDC_SHADOWBUF_COMBO, NULL, NULL);
            {
                int sel = 0;
                for (size_t i = 0; i < NUM_SHADOWBUF_CHOICES; i++) {
                    SendMessageA(g_hShadowBufCombo, CB_ADDSTRING, 0, (LPARAM)g_shadowBufLabels[i]);
                    if (g_shadowBufChoices[i] == g_shadowBufResPct) sel = (int)i;
                }
                SendMessageA(g_hShadowBufCombo, CB_SETCURSEL, sel, 0);
            }

            // MSAA + alpha-to-coverage. Both apply within a frame: the
            // substitution reads the sample count per bind, and A2C mirrors
            // the next ALPHATESTENABLE write. Surface rebuild happens on the
            // render thread, never here.
            int y4c = y4b + 30;
            CreateWindowA("STATIC", "MSAA (needs native res):", WS_CHILD | WS_VISIBLE,
                10, y4c + 4, 150, 20, hwnd, NULL, NULL, NULL);
            g_hMsaaCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                165, y4c, 235, 200, hwnd, (HMENU)(UINT_PTR)IDC_MSAA_COMBO, NULL, NULL);
            {
                int sel = 0;
                for (size_t i = 0; i < NUM_MSAA_CHOICES; i++) {
                    SendMessageA(g_hMsaaCombo, CB_ADDSTRING, 0, (LPARAM)g_msaaLabels[i]);
                    if (g_msaaChoices[i] == g_msaaSamples) sel = (int)i;
                }
                SendMessageA(g_hMsaaCombo, CB_SETCURSEL, sel, 0);
            }
            // A2C checkbox RETIRED from the panel: 85/256 of this engine's
            // pixel shaders do their cutouts with in-shader TEXKILL and the
            // engine never writes ALPHATESTENABLE (a2c stayed 0 across whole
            // sessions), so the vendor A2C backdoor has nothing to act on.
            // Working A2C here means patching the cutout shaders - parked
            // with the planned shader-injection work. Config key and mirror
            // code remain, inert.
            //
            // In its place: the post-shader kill list. Substitutes the chosen
            // candidate's pixel shader at BIND time - passthrough kills the
            // filter; with the magenta debug toggle on it paints the pass's
            // footprint instead, which is the instrument for FINDING the
            // real AA pass among the candidates.
            // NO GUI for Graphics_Scaling: it is already a graphics option in
            // the game's own menu, so a duplicate control here is pure
            // clutter in a panel that was deliberately trimmed. The config
            // key and ApplyScalingMode remain (harmless, default 0 = don't
            // touch) only because the [scaling] log line reporting the
            // engine's current mode is worth keeping.
#if ENABLE_SHADER_DIAG
            // Identify walk - hot. Selecting N paints that shader magenta and
            // the label names it. Retired from the shipping panel with the
            // rest of the diagnostics; one #define brings it back.
            int y4f = y4c + 30;
            CreateWindowA("STATIC", "Identify shader:", WS_CHILD | WS_VISIBLE,
                10, y4f + 4, 150, 20, hwnd, NULL, NULL, NULL);
            g_hA2cIdCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                165, y4f, 235, 260, hwnd, (HMENU)(UINT_PTR)IDC_A2CID_COMBO, NULL, NULL);
            {
                // Only "Off" up front - real entries are appended by the
                // timer as the game creates shaders, so the list grows but
                // existing indices never move.
                SendMessageA(g_hA2cIdCombo, CB_ADDSTRING, 0, (LPARAM)"Off");
                SendMessageA(g_hA2cIdCombo, CB_SETCURSEL, 0, 0);
            }
            int y4g = y4f + 28;
            g_hA2cIdLabel = CreateWindowA("STATIC", "(no candidates yet)",
                WS_CHILD | WS_VISIBLE, 10, y4g, 390, 18, hwnd, NULL, NULL, NULL);

            int y4d = y4g + 24;
            CreateWindowA("STATIC", "Kill post shader:", WS_CHILD | WS_VISIBLE,
                10, y4d + 4, 150, 20, hwnd, NULL, NULL, NULL);
            g_hFxaaCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                165, y4d, 235, 220, hwnd, (HMENU)(UINT_PTR)IDC_FXAA_COMBO, NULL, NULL);
            {
                for (size_t i = 0; i <= NUM_PS_KILL; i++)
                    SendMessageA(g_hFxaaCombo, CB_ADDSTRING, 0, (LPARAM)g_psKillLabels[i]);
                LONG p = g_fxaaPick;
                if (p < 0 || (size_t)p > NUM_PS_KILL) p = 0;
                SendMessageA(g_hFxaaCombo, CB_SETCURSEL, (WPARAM)p, 0);
            }
#else
            int y4d = y4c;   // diagnostic rows retired; the panel closes up
#endif

            // Cascade splits. These apply per frame, so changing them here is
            // visible immediately - no relaunch and no area change needed,
            // unlike the shadow resolution above.
            int y5 = y4d + 30;
            CreateWindowA("STATIC", "Shadow cascade near split:", WS_CHILD | WS_VISIBLE,
                10, y5 + 4, 160, 20, hwnd, NULL, NULL, NULL);
            g_hSplitNearCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                175, y5, 225, 200, hwnd, (HMENU)(UINT_PTR)IDC_SPLITNEAR_COMBO, NULL, NULL);
            int y6 = y5 + 30;
            CreateWindowA("STATIC", "Shadow cascade far split:", WS_CHILD | WS_VISIBLE,
                10, y6 + 4, 160, 20, hwnd, NULL, NULL, NULL);
            g_hSplitFarCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                175, y6, 225, 200, hwnd, (HMENU)(UINT_PTR)IDC_SPLITFAR_COMBO, NULL, NULL);
            {
                int selN = 0, selF = 0;
                for (size_t i = 0; i < NUM_SPLIT_CHOICES; i++) {
                    SendMessageA(g_hSplitNearCombo, CB_ADDSTRING, 0, (LPARAM)g_splitPctLabels[i]);
                    SendMessageA(g_hSplitFarCombo,  CB_ADDSTRING, 0, (LPARAM)g_splitPctLabels[i]);
                    if (g_splitPctChoices[i] == g_shadowSplitNearPct) selN = (int)i;
                    if (g_splitPctChoices[i] == g_shadowSplitFarPct)  selF = (int)i;
                }
                SendMessageA(g_hSplitNearCombo, CB_SETCURSEL, selN, 0);
                SendMessageA(g_hSplitFarCombo,  CB_SETCURSEL, selF, 0);
            }

#if ENABLE_TALK_TIMER
            // Talk-entry countdown rate. Takes effect on the next frame once
            // patched, so a bugged interaction can be retried without a
            // relaunch.
            int y7 = y6 + 30;
            CreateWindowA("STATIC", "Talk timer rate (60fps fix):", WS_CHILD | WS_VISIBLE,
                10, y7 + 4, 165, 20, hwnd, NULL, NULL, NULL);
            g_hTalkCombo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                180, y7, 220, 200, hwnd, (HMENU)(UINT_PTR)IDC_TALKTIMER_COMBO, NULL, NULL);
            {
                int sel = 0;
                for (size_t i = 0; i < NUM_TALK_CHOICES; i++) {
                    SendMessageA(g_hTalkCombo, CB_ADDSTRING, 0, (LPARAM)g_talkPctLabels[i]);
                    if (g_talkPctChoices[i] == g_talkTimerPct) sel = (int)i;
                }
                SendMessageA(g_hTalkCombo, CB_SETCURSEL, sel, 0);
            }
#else
            // Talk-timer control retired: nothing sits between the split
            // combos and the stats label, so the label moves up into the freed
            // row instead of leaving a gap.
            int y7 = y6;
#endif

            g_hStatsLabel = CreateWindowA("STATIC", "frametime: (waiting)",
                WS_CHILD | WS_VISIBLE,
                10, y7 + 30, 400, 56, hwnd, NULL, NULL, NULL);
        }
        SetTimer(hwnd, IDT_SYNC_TIMER, 250, NULL);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDC_THRESH_APPLY) {
            char buf[32];
            GetWindowTextA(g_hThreshEdit, buf, sizeof(buf));
            double ms = atof(buf);
            LONG us = (LONG)(ms * 1000.0 + 0.5);
            // Clamp to the same bounds the config loader enforces, then write
            // the clamped value back into the box so the UI never shows a
            // number that is not actually in effect.
            if (us < g_numerics[0].lo) us = g_numerics[0].lo;
            if (us > g_numerics[0].hi) us = g_numerics[0].hi;
            g_stutterThresholdUsec = us;
            sprintf(buf, "%.1f", us / 1000.0);
            SetWindowTextA(g_hThreshEdit, buf);
            SaveConfig();
            char line[128];
            sprintf(line, "[gui] stutter/graph threshold set to %ldus (%.1fms)", us, us / 1000.0);
            LogLine(line);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDC_FPS_APPLY) {
            char buf[32];
            GetWindowTextA(g_hFpsEdit, buf, sizeof(buf));
            double fps = atof(buf);
            LONG x100 = (fps <= 0.0) ? 0 : (LONG)(fps * 100.0 + 0.5);
            if (x100 < g_numerics[1].lo) x100 = g_numerics[1].lo;
            if (x100 > g_numerics[1].hi) x100 = g_numerics[1].hi;
            g_targetFpsX100 = x100;
            sprintf(buf, "%.2f", x100 / 100.0);
            SetWindowTextA(g_hFpsEdit, buf);
            SaveConfig();
            char line[160];
            if (x100 > 0) {
                sprintf(line, "[gui] target FPS set to %.2f - reuses the game's own limiter, "
                              "CPU cost from UnlockFramerate WILL return", x100 / 100.0);
            } else {
                sprintf(line, "[gui] target FPS set to unlocked (0)");
            }
            LogLine(line);
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE &&
            (LOWORD(wParam) == IDC_SPLITNEAR_COMBO || LOWORD(wParam) == IDC_SPLITFAR_COMBO)) {
            int isNear = (LOWORD(wParam) == IDC_SPLITNEAR_COMBO);
            HWND h = isNear ? g_hSplitNearCombo : g_hSplitFarCombo;
            int sel = (int)SendMessageA(h, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && (size_t)sel < NUM_SPLIT_CHOICES) {
                LONG v = g_splitPctChoices[sel];
                if (isNear) g_shadowSplitNearPct = v; else g_shadowSplitFarPct = v;
                SaveConfig();
                char line[160];
                sprintf(line, "[gui] cascade %s split set to %ld%% (applies immediately)",
                        isNear ? "near" : "far", v);
                LogLine(line);
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_MSAA_COMBO) {
            int sel = (int)SendMessageA(g_hMsaaCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && (size_t)sel < NUM_MSAA_CHOICES) {
                g_msaaSamples = g_msaaChoices[sel];
                SaveConfig();
                char line[224];
                sprintf(line, "[gui] MSAA set to %ld - applies within a frame; surfaces are "
                              "rebuilt on the render thread. Engages only when in-game res "
                              "matches the desktop res.", g_msaaSamples);
                LogLine(line);
            }
            return 0;
        }
#if ENABLE_SHADER_DIAG
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_A2CID_COMBO) {
            int sel = (int)SendMessageA(g_hA2cIdCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel <= PS_IDENTIFY_LIST_MAX) {
                g_psIdentify = sel;
                SaveConfig();
                char line[160];
                sprintf(line, "[gui] shader identify -> rank %d%s", sel,
                        sel ? " (magenta)" : " (off)");
                LogLine(line);
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_FXAA_COMBO) {
            int sel = (int)SendMessageA(g_hFxaaCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && (size_t)sel <= NUM_PS_KILL) {
                g_fxaaPick = sel;
                SaveConfig();
                char line[224];
                sprintf(line, "[gui] post-shader kill set to %s%s", g_psKillLabels[sel],
                        (sel > 0 && g_msaaDebugClear)
                            ? "  (magenta debug ON: footprint mode)" : "");
                LogLine(line);
            }
            return 0;
        }
#endif  // ENABLE_SHADER_DIAG
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_SHADOWBUF_COMBO) {
            int sel = (int)SendMessageA(g_hShadowBufCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && (size_t)sel < NUM_SHADOWBUF_CHOICES) {
                g_shadowBufResPct = g_shadowBufChoices[sel];
                SaveConfig();
                char line[256];
                sprintf(line, "[gui] shadow buffer res set to %ld%% of screen - applies when the "
                              "buffers are next created (area change or restart). Engine default "
                              "is 50%%; gains continue past 100%% because the limit is aliasing, "
                              "not filter softness.", g_shadowBufResPct);
                LogLine(line);
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_SHADOW_COMBO) {
            int sel = (int)SendMessageA(g_hShadowCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && (size_t)sel < NUM_SHADOW_CHOICES) {
                g_shadowMapRes = g_shadowResChoices[sel];
                SaveConfig();
                char line[224];
                // The engine reads this value both when it CREATES the shadow
                // textures and every frame for the projection and PCF offsets.
                // Changing it mid-session updates the maths immediately while
                // the existing textures keep their old size, so shadows can
                // look wrong until the set is rebuilt (area change / device
                // reset). Say so rather than let it look like a broken fix.
                sprintf(line, "[gui] shadow map resolution set to %ld - takes full effect when the "
                              "shadow maps are next recreated (area change or restart); may look "
                              "wrong until then", g_shadowMapRes);
                LogLine(line);
            }
            return 0;
        }
#if ENABLE_TALK_TIMER
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_TALKTIMER_COMBO) {
            int sel = (int)SendMessageA(g_hTalkCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && (size_t)sel < NUM_TALK_CHOICES) {
                g_talkTimerPct = g_talkPctChoices[sel];
                SaveConfig();
                char line[224];
                // Once the instruction is patched the rate follows this value
                // live, but the patch itself is one-way for the session: going
                // back to Off leaves the operand pointing at our double, which
                // then has to hold the stock 0.05 rather than be ignored.
                if (g_talkTimerPct <= 0 && g_talkStepPatched == 1) g_talkStep = 0.05;
                sprintf(line, "[gui] talk timer rate set to %ld%% (step %.4f/frame, "
                              "entries live %.0f-%.0f frames) - applies to the next "
                              "conversation", g_talkTimerPct,
                        g_talkTimerPct > 0 ? 0.05 * g_talkTimerPct / 100.0 : 0.05,
                        5.0 / (g_talkTimerPct > 0 ? 0.05 * g_talkTimerPct / 100.0 : 0.05),
                        10.0 / (g_talkTimerPct > 0 ? 0.05 * g_talkTimerPct / 100.0 : 0.05));
                LogLine(line);
            }
            return 0;
        }
#endif  // ENABLE_TALK_TIMER
#if ENABLE_DEFER_UPLOADS
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDC_DEFER_APPLY) {
            char buf[32];
            GetWindowTextA(g_hDeferEdit, buf, sizeof(buf));
            LONG n = atol(buf);
            if (n < g_numerics[2].lo) n = g_numerics[2].lo;
            if (n > g_numerics[2].hi) n = g_numerics[2].hi;
            g_deferPerFrame = n;
            sprintf(buf, "%ld", n);
            SetWindowTextA(g_hDeferEdit, buf);
            SaveConfig();
            char line[128];
            sprintf(line, "[gui] deferred uploads per frame set to %ld", n);
            LogLine(line);
            return 0;
        }
#endif
        if (HIWORD(wParam) == BN_CLICKED) {
            size_t idx = (size_t)(LOWORD(wParam) - IDC_CHECK_BASE);
            if (idx < NUM_TOGGLES) {
                LONG newVal = (SendMessageA(g_hCheckBoxes[idx], BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
                *g_toggles[idx].flag = newVal;
                SaveConfig();
                char line[200];
                sprintf(line, "[gui] %s now %s", g_toggles[idx].label, newVal ? "ENABLED" : "DISABLED");
                LogLine(line);
            }
        }
        return 0;
    case WM_TIMER:
        // Keeps the checkboxes truthful regardless of which control path
        // (hotkey or this window) last changed the flags - a hotkey press
        // updates the underlying value immediately; this just re-syncs the
        // checkbox visuals to match within 250ms.
        if (wParam == IDT_SYNC_TIMER) {
            for (size_t i = 0; i < NUM_TOGGLES; i++) {
                SendMessageA(g_hCheckBoxes[i], BM_SETCHECK,
                             *g_toggles[i].flag ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            // F9 = capture the FULL scene target at full resolution, right
            // now. A timer-armed capture cannot be aimed: the frame that
            // matters is whatever the user is looking at when they see the
            // effect, and so far every automatic capture has landed on the
            // wrong subject. Polled here (250ms) rather than from the render
            // thread; the render thread performs the actual capture.
#if ENABLE_SHADER_DIAG
            // Grow the identify list as the game creates shaders. APPEND-ONLY,
            // so an index selected a minute ago still means the same shader -
            // the whole point of moving off activity ranking.
            if (g_hA2cIdCombo) {
                LONG have = (LONG)SendMessageA(g_hA2cIdCombo, CB_GETCOUNT, 0, 0) - 1;
                LONG want = g_psMapCount;
                if (want > PS_IDENTIFY_LIST_MAX) want = PS_IDENTIFY_LIST_MAX;
                if (have < 0) have = 0;
                for (LONG i = have; i < want; i++) {
                    char it[64];
                    sprintf(it, "#%ld  ps_%08X%s", i + 1, g_psMap[i].hash,
                            g_psMap[i].hasVariant ? "  CUTOUT" : "");
                    SendMessageA(g_hA2cIdCombo, CB_ADDSTRING, 0, (LPARAM)it);
                }
            }
            // Readout for the SELECTED entry. "drawing now" is what tells you
            // whether the thing you are looking at is even on screen, which
            // matters when stepping a long static list.
            if (g_hA2cIdLabel) {
                char st[192];
                LONG want = g_psIdentify;
                if (want == 0) {
                    sprintf(st, "off - %ld shaders known, %ld drawing now",
                            g_psMapCount, g_psRankCount);
                } else if (want <= g_psMapCount) {
                    LONG idx = want - 1;
                    sprintf(st, "#%ld ps_%08X  draws/s=%ld  taps=%u  %s",
                            want, g_psMap[idx].hash, g_psMap[idx].recent * 2,
                            g_psMap[idx].taps,
                            g_psMap[idx].hasVariant ? "CUTOUT (A2C-able)" : "not a cutout");
                } else {
                    sprintf(st, "#%ld: not created yet (%ld known)",
                            want, g_psMapCount);
                }
                SetWindowTextA(g_hA2cIdLabel, st);
            }
#endif  // ENABLE_SHADER_DIAG
#if ENABLE_SURFACE_DIAG
            {
                static int f9WasDown = 0;
                int f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
                if (f9 && !f9WasDown) {
                    g_captureRequest = 1;
                    LogLine("[capture] F9 pressed - full-frame capture armed");
                }
                f9WasDown = f9;
            }
#endif  // ENABLE_SURFACE_DIAG
            if (g_hStatsLabel) {
                LONG p50 = g_liveP50, p99 = g_liveP99;
                LONG n = g_liveFrames, over = g_liveOver16, worst = g_liveWorst;
                char st[256];
                sprintf(st, "ENGINE TICK (not RTSS/present):\r\n"
                            "  %.1f fps   p50 %.1fms   p99 %.1fms\r\n"
                            "  worst %.1fms   over 16.7ms: %ld/%ld",
                        p50 > 0 ? 1000000.0 / (double)p50 : 0.0,
                        p50 / 1000.0, p99 / 1000.0, worst / 1000.0, over, n);
                SetWindowTextA(g_hStatsLabel, st);
            }
            // (The overlay show/hide/repaint drive used to live here; it is
            // OverlayThread's job now so the overlay survives this window
            // being retired.)
        }
        return 0;
    case WM_CLOSE:
        // Hide rather than destroy - an accidental close shouldn't lose the
        // panel or affect the hooks, which keep running either way since
        // the flags they read live independently of this window.
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static DWORD WINAPI GuiThread(LPVOID param)
{
    (void)param;
    LogLine("[boot] gui: thread entered");
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = PanelWndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "LRStutterFixPanel";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);

    int panelHeight = 10 + (int)NUM_TOGGLES * 26 + 6 + 30 * 10 + 28 + 24 + 56 + 46;  // +threshold +fps +shadow rows +msaa +a2c identify(+label) +kill +stats
    HWND hwnd = CreateWindowExA(WS_EX_TOPMOST, "LRStutterFixPanel", "LR Stutter Fix - Debug Panel",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        10, 10, 420, panelHeight, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) { LogLine("[boot] gui: CreateWindow FAILED"); return 0; }
    LogLine("[boot] gui: panel created");
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

#endif  // ENABLE_GUI_PANEL

