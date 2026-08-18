// ---- In-game AO tuning panel (2026-08-18) ----------------------------------
//
// WHY THIS EXISTS: the Win32 tuning window fought fullscreen and lost, three
// fixes in a row. Each fix uncovered the next OS behaviour working against a
// top-level window over an exclusive-ish D3D9 game:
//
//   - topmost is a BAND, not a guarantee - the game re-asserts on every focus
//     change and covers the panel (63fdf75 tried ownership);
//   - ownership pins the Z-order but couples fates - Windows HIDES owned
//     windows when the owner minimises, and a fullscreen device minimises on
//     ANY focus loss, so dragging the unfocused panel made it vanish
//     (35424ed tried MA_NOACTIVATE);
//   - and every activation change near the game window costs it a device
//     focus-loss round-trip, which the user reads as a fat stutter on every
//     window switch.
//
// The user called the resolution: stop fighting the window manager, render
// the UI inside the frame at Present time, the way ReShade does. A panel
// that is PART of the frame cannot be covered, cannot be minimised, cannot
// steal focus, and costs the window manager nothing.
//
// DESIGN, chosen for robustness over cleverness:
//
//   Render  - GDI draws the whole panel (background, text, tracks, thumbs)
//             into a 32bpp DIB every frame while open. That is ~20 DrawText
//             calls into system memory - microseconds - and it buys exact
//             CJK text for free through GDI font linking, which matters
//             because every label here is localised (S_* through TR()).
//             The DIB is then copied into a D3D9 texture and drawn as ONE
//             pre-transformed quad. No glyph atlas, no vertex text, nothing
//             to break when a label changes language mid-session.
//   Input   - polled: GetCursorPos mapped into backbuffer space plus
//             GetAsyncKeyState(VK_LBUTTON). No window messages exist to
//             steal focus or run modal loops. The game keeps the foreground
//             the entire time, which is the point.
//   Draws   - through g_orig* pointers only (self-interference rule), with
//             engine state restored EXPLICITLY from the shadows in 15
//             (state-block rule: CreateStateBlock+Apply is destructive on
//             this device). Same discipline as the AO passes, same shadows.
//
// The Win32 window stays behind AoPanelInGame=0 as the fallback until this
// has earned trust - retired then, per the gate rule, not deleted.

#if ENABLE_AO_SSAO

// ---- geometry (texture space, 1:1 with backbuffer pixels) ------------------
#define IG_W        428
#define IG_TITLE_H  24
#define IG_ROW_H    34
#define IG_LABEL_W  120
#define IG_TRACK_W  230
#define IG_VAL_W    50
#define IG_PAD      10
// rows + raw checkbox + resolution row + reset button + bottom pad
#define IG_H (IG_TITLE_H + IG_PAD + (int)AO_ROWS * IG_ROW_H + 22 + 26 + 26 + IG_PAD)
// The open dropdown extends PAST the panel's normal bottom, exactly as a real
// one does. The texture is always allocated tall enough for that; only the
// drawn height changes, so opening the list costs nothing but a taller quad.
#define IG_DROP_ITEM_H 22
#define IG_DROP_N      3
#define IG_DROP_EXTRA  (IG_DROP_ITEM_H * IG_DROP_N + 12)
#define IG_TEX_H       (IG_H + IG_DROP_EXTRA)
// The white patch the cursor quads sample: bottom-right corner, drawn last so
// nothing paints over it. 8x8 so bilinear can never bleed an edge in.
#define IG_WHITE_X  (IG_W - 8)
#define IG_WHITE_Y  (IG_H - 8)

// (g_inGameUi is declared in 01_config_gates.c - the config table in 08
// and the Win32 gate in 10 both sit earlier in the TU and need it.)

// Panel position in backbuffer pixels. Session-only on purpose for now: the
// backbuffer size can differ between sessions (resolution change), and a
// stale persisted position could park the panel off-screen with no window
// manager to drag it back. Re-centre-ish every boot instead.
static LONG g_igX = 80, g_igY = 80;

// Interaction state, render thread only (IgPresent is the sole writer).
static int  g_igPrevDown = 0;
// 0 none, 1 = AO title bar, 2+i = AO slider row i, and the two displays.
#define IG_DRAG_OVL  90
#define IG_DRAG_STAT 91
static int  g_igDrag = 0;
static LONG g_igDragOffX = 0, g_igDragOffY = 0;
static DWORD g_igResetArm = 0;     // tick count of first reset click, 0 = disarmed
static LONG g_igOpenLogged = 0;

// GDI side.
static HDC     g_igDc = NULL;
static HBITMAP g_igBmp = NULL;
static void   *g_igBits = NULL;    // 32bpp top-down, IG_W x IG_TEX_H
static HFONT   g_igFont = NULL, g_igFontBold = NULL;

// D3D side.
static IDirect3DTexture9 *g_igTex = NULL;
static IDirect3DPixelShader9 *g_igPs = NULL;
static LONG g_igPsState = 0;

typedef struct { float x, y, z, rhw, u, v; } IgVtx;

// ---- layout helpers (shared by paint and hit-test, so they cannot skew) ----
static void IgRowTrack(int i, RECT *r)
{
    r->left = IG_PAD + IG_LABEL_W;
    r->top = IG_TITLE_H + IG_PAD + i * IG_ROW_H + 8;
    r->right = r->left + IG_TRACK_W;
    r->bottom = r->top + 16;
}
static void IgCheckRect(RECT *r)
{
    r->left = IG_PAD; r->top = IG_TITLE_H + IG_PAD + (int)AO_ROWS * IG_ROW_H + 2;
    r->right = IG_W - IG_PAD; r->bottom = r->top + 20;
}
// AO resolution: a REAL dropdown. Two earlier shapes were wrong for the same
// underlying reason - I kept treating "dropdown" as a Win32 combo box, which
// needs a popup, which is a window, which is the species this panel exists to
// escape. That constraint does not exist here. In an immediate-mode UI drawn
// into our own texture, an open list is just more pixels painted later and
// hit-tested first; ReShade's ImGui does exactly this. A cycle button, then
// three ratio segments, were both workarounds for a limit that was imagined.
//
// It matters beyond tidiness: the segments could only fit "1:1", which threw
// away TR(S_RES_NATIVE) - "Native (expensive at high res)" - and that warning
// is the single most useful thing on this control, since Native at 4K is the
// setting most likely to cost someone their framerate. The list shows every
// option at full translated length.
static int g_igDropOpen = 0;

static void IgResRect(RECT *r)
{
    r->left = IG_PAD + IG_LABEL_W;
    r->right = r->left + IG_TRACK_W + 8 + IG_VAL_W;
    r->top = IG_TITLE_H + IG_PAD + (int)AO_ROWS * IG_ROW_H + 24;
    r->bottom = r->top + 22;
}
static void IgDropItemRect(int i, RECT *r)
{
    IgResRect(r);
    r->top = r->bottom + 2 + i * IG_DROP_ITEM_H;
    r->bottom = r->top + IG_DROP_ITEM_H;
}
// Drawn height: the list extends past the normal bottom while open.
static int IgPanelH(void) { return IG_H + (g_igDropOpen ? IG_DROP_EXTRA : 0); }
static void IgResetRect(RECT *r)
{
    r->left = IG_PAD + IG_LABEL_W;
    r->top = IG_TITLE_H + IG_PAD + (int)AO_ROWS * IG_ROW_H + 50;
    r->right = r->left + IG_TRACK_W + 8 + IG_VAL_W; r->bottom = r->top + 22;
}
static void IgCloseRect(RECT *r)
{
    r->left = IG_W - IG_TITLE_H; r->top = 0; r->right = IG_W; r->bottom = IG_TITLE_H;
}

static int IgEnsureGdi(void)
{
    if (g_igDc) return 1;
    BITMAPINFO bi;
    HDC screen = GetDC(NULL);
    if (!screen) return 0;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = IG_W;
    bi.bmiHeader.biHeight = -IG_TEX_H;          // top-down, so rows match the lock copy
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_igDc = CreateCompatibleDC(screen);
    g_igBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_igBits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!g_igDc || !g_igBmp || !g_igBits) return 0;
    SelectObject(g_igDc, g_igBmp);
    // Segoe UI carries the Latin scripts and GDI font linking pulls in the
    // CJK faces (Yu Gothic / MingLiU / Malgun) behind it - the same mechanism
    // dialogs use, which is why the Win32 window never had to think about it.
    g_igFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    g_igFontBold = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
    return 1;
}

static void IgFill(RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(g_igDc, r, b);
    DeleteObject(b);
}

static void IgText(int x, int y, int w, const wchar_t *s, COLORREF c, HFONT f, UINT align)
{
    RECT r = { x, y, x + w, y + 20 };
    SelectObject(g_igDc, f);
    SetTextColor(g_igDc, c);
    DrawTextW(g_igDc, s, -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | align);
}

// Paint the whole panel into the DIB. Runs every open frame - deliberately no
// dirty tracking: ~20 GDI calls into system memory is nothing, and "always
// current" removes an entire class of stale-widget bugs the Win32 version
// needed BM_SETCHECK re-seeding calls to paper over (three separate sites).
static void IgPaint(int mx, int my)
{
    RECT r;
    wchar_t buf[64];
    LONG est = (g_aoEnable == 2) ? 1 : 0;

    SetBkMode(g_igDc, TRANSPARENT);
    r.left = 0; r.top = 0; r.right = IG_W; r.bottom = IgPanelH();
    IgFill(&r, RGB(28, 30, 34));
    r.bottom = IG_TITLE_H;
    IgFill(&r, RGB(46, 50, 58));
    IgText(IG_PAD, 2, IG_W - IG_TITLE_H - IG_PAD, AoTweakTitle(),
           RGB(235, 235, 235), g_igFontBold, DT_LEFT);
    // Close box.
    IgCloseRect(&r);
    if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom)
        IgFill(&r, RGB(140, 40, 40));
    IgText(r.left, 2, r.right - r.left, L"\x2715", RGB(220, 220, 220),
           g_igFont, DT_CENTER);

    for (size_t i = 0; i < AO_ROWS; i++) {
        RECT t;
        LONG v = *g_aoRows[i].val, lo = g_aoRows[i].lo, hi = g_aoRows[i].hi;
        IgRowTrack((int)i, &t);
        IgText(IG_PAD, t.top - 4, IG_LABEL_W - 4, TR(g_aoRows[i].nameId),
               RGB(210, 210, 210), g_igFont, DT_LEFT);
        // Track, then thumb. The thumb is 10px wide, centred on the value.
        r = t; r.top += 5; r.bottom -= 5;
        IgFill(&r, RGB(58, 62, 70));
        if (hi > lo) {
            int cx = t.left + (int)((__int64)(v - lo) * (IG_TRACK_W - 10) / (hi - lo)) + 5;
            r.left = cx - 5; r.right = cx + 5; r.top = t.top; r.bottom = t.bottom;
            IgFill(&r, (g_igDrag == 2 + (int)i) ? RGB(120, 170, 255) : RGB(150, 150, 160));
        }
        _snwprintf(buf, 64, L"%ld", v);
        buf[63] = 0;
        IgText(t.right + 8, t.top - 4, IG_VAL_W, buf, RGB(235, 235, 235), g_igFont, DT_RIGHT);
    }

    // Raw-view checkbox: box + tick + label.
    IgCheckRect(&r);
    {
        RECT box = { r.left, r.top + 3, r.left + 14, r.top + 17 };
        IgFill(&box, RGB(58, 62, 70));
        if (g_aoRawView) {
            RECT in = { box.left + 3, box.top + 3, box.right - 3, box.bottom - 3 };
            IgFill(&in, RGB(120, 170, 255));
        }
        IgText(r.left + 20, r.top, r.right - r.left - 20, TR(S_SHOW_RAW),
               RGB(210, 210, 210), g_igFont, DT_LEFT);
    }

    // Resolution: closed dropdown here; the open list is painted LAST, below,
    // so it overlays whatever it covers by simple painter's order.
    {
        int active = AoResIndexOf(g_aoResDiv);
        IgResRect(&r);
        IgText(IG_PAD, r.top + 1, IG_LABEL_W - 4, TR(S_AO_RES),
               RGB(210, 210, 210), g_igFont, DT_LEFT);
        IgFill(&r, (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom)
                       ? RGB(66, 72, 84) : RGB(52, 56, 64));
        IgText(r.left + 6, r.top + 1, r.right - r.left - 24,
               TR(active == 0 ? S_RES_NATIVE : active == 1 ? S_RES_HALF : S_RES_QUARTER),
               RGB(225, 225, 225), g_igFont, DT_LEFT);
        IgText(r.right - 20, r.top + 1, 14, g_igDropOpen ? L"\x25B2" : L"\x25BC",
               RGB(180, 184, 192), g_igFont, DT_CENTER);
    }

    // Reset, double-click armed: a MessageBox is a window AND a modal loop on
    // whichever thread shows it - both banned here. First click arms for 3s
    // and says so on the button itself; second click within the window fires.
    IgResetRect(&r);
    {
        int armed = g_igResetArm && (GetTickCount() - g_igResetArm < 3000);
        IgFill(&r, armed ? RGB(150, 60, 40)
                         : (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom)
                               ? RGB(66, 72, 84) : RGB(52, 56, 64));
        if (armed) {
            _snwprintf(buf, 64, L"%s ?", TR(S_RESET_AO));
            buf[63] = 0;
        }
        IgText(r.left, r.top + 1, r.right - r.left, armed ? buf : TR(S_RESET_AO),
               RGB(225, 225, 225), g_igFont, DT_CENTER);
    }

    // The open dropdown list, painted after everything else so it overlays the
    // reset button beneath it - painter's order IS the Z-order here, which is
    // the whole reason an in-frame UI can do this at all.
    if (g_igDropOpen) {
        int active = AoResIndexOf(g_aoResDiv);
        RECT box;
        IgDropItemRect(0, &box);
        box.top -= 2;
        IgDropItemRect(IG_DROP_N - 1, &r);
        box.bottom = r.bottom + 2;
        box.left -= 2; box.right += 2;
        IgFill(&box, RGB(70, 76, 88));                 // 2px border
        for (int i = 0; i < IG_DROP_N; i++) {
            int hot;
            IgDropItemRect(i, &r);
            hot = (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom);
            IgFill(&r, hot ? RGB(58, 108, 178)
                           : (i == active) ? RGB(44, 62, 88) : RGB(38, 41, 47));
            IgText(r.left + 6, r.top + 1, r.right - r.left - 10,
                   TR(i == 0 ? S_RES_NATIVE : i == 1 ? S_RES_HALF : S_RES_QUARTER),
                   (hot || i == active) ? RGB(255, 255, 255) : RGB(200, 200, 205),
                   (i == active) ? g_igFontBold : g_igFont, DT_LEFT);
        }
    }

    // The cursor's white patch, painted last so nothing covers it. Inside the
    // ALWAYS-drawn region (above IG_H), so the cursor still has a source when
    // the dropdown is shut and the quad is short.
    r.left = IG_WHITE_X; r.top = IG_WHITE_Y; r.right = IG_W; r.bottom = IG_H;
    IgFill(&r, RGB(255, 255, 255));
}

// ---- input ------------------------------------------------------------------
// The game window, cached. GameMenuFindWindow ENUMERATES every top-level
// window in the system; calling it per EndScene (with g_gameHwnd null, which
// it permanently is in this game) was a measurable slice of the 20fps report.
// Revalidated cheaply with IsWindow and re-resolved once a second at most.
static HWND IgGameWindow(void)
{
    static HWND cached = NULL;
    static DWORD lastLookup = 0;
    DWORD now;
    if (g_gameHwnd) return g_gameHwnd;
    if (cached && IsWindow(cached)) return cached;
    now = GetTickCount();
    if (now - lastLookup < 1000) return cached;
    lastLookup = now;
    cached = GameMenuFindWindow();
    return cached;
}

// Backbuffer-space cursor. Client coords scale by backbuffer/client because
// borderless keeps the backbuffer at desktop size while the client can be
// anything; at 1:1 the scale is identity and this is a no-op.
static int IgCursor(LONG *ox, LONG *oy)
{
    POINT p;
    RECT rc;
    HWND w = IgGameWindow();
    if (!w || !GetCursorPos(&p) || !ScreenToClient(w, &p)) return 0;
    if (!GetClientRect(w, &rc) || rc.right <= 0 || rc.bottom <= 0) return 0;
    if (g_backbufW > 1 && g_backbufH > 1) {
        p.x = (LONG)((__int64)p.x * g_backbufW / rc.right);
        p.y = (LONG)((__int64)p.y * g_backbufH / rc.bottom);
    }
    *ox = p.x; *oy = p.y;
    return 1;
}

static void IgInput(LONG mx, LONG my)
{
    int down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    int click = down && !g_igPrevDown;
    LONG lx = mx - g_igX, ly = my - g_igY;
    RECT r;

    if (g_igDrag == 1) {                             // panel being dragged
        g_igX = mx - g_igDragOffX;
        g_igY = my - g_igDragOffY;
        if (!down) g_igDrag = 0;
        g_igPrevDown = down;
        return;
    }
    // The two display surfaces drag from anywhere on them, exactly as their
    // Win32 versions did (both answered WM_NCHITTEST with HTCAPTION). Position
    // is saved on release, into the same ini keys as before.
    if (g_igDrag == IG_DRAG_OVL || g_igDrag == IG_DRAG_STAT) {
        LONG *px = (g_igDrag == IG_DRAG_OVL) ? &g_overlayX : &g_statusX;
        LONG *py = (g_igDrag == IG_DRAG_OVL) ? &g_overlayY : &g_statusY;
        InterlockedExchange(px, mx - g_igDragOffX);
        InterlockedExchange(py, my - g_igDragOffY);
        if (!down) { g_igDrag = 0; SaveConfig(); }
        g_igPrevDown = down;
        return;
    }
    if (g_igDrag >= 2) {                             // slider being dragged
        size_t i = (size_t)(g_igDrag - 2);
        RECT t;
        IgRowTrack((int)i, &t);
        LONG lo = g_aoRows[i].lo, hi = g_aoRows[i].hi;
        LONG v = lo + (LONG)((__int64)(lx - t.left) * (hi - lo) / (IG_TRACK_W - 1));
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        InterlockedExchange(g_aoRows[i].val, v);
        if (!down) {
            g_igDrag = 0;
            SaveConfig();                            // once per drag, like SB_ENDSCROLL
        }
        g_igPrevDown = down;
        return;
    }

    // An open list swallows the next click wherever it lands - selecting an
    // item, or closing without changing anything. Handled BEFORE the controls
    // below so a click on the list cannot also fall through to whatever it is
    // drawn over (the reset button sits under it, which would be a nasty
    // accident).
    if (g_igDropOpen && click) {
        g_igDropOpen = 0;
        for (int i = 0; i < IG_DROP_N; i++) {
            IgDropItemRect(i, &r);
            if (lx >= r.left && lx < r.right && ly >= r.top && ly < r.bottom) {
                if (g_aoResDivs[i] != g_aoResDiv) {
                    char l[64];
                    InterlockedExchange(&g_aoResDiv, g_aoResDivs[i]);
                    sprintf(l, "[ssao] AO resolution: 1/%ld (panel)", g_aoResDiv);
                    LogLine(l);
                    SaveConfig();
                }
                break;
            }
        }
        g_igPrevDown = down;
        return;
    }

    if (click && lx >= 0 && lx < IG_W && ly >= 0 && ly < IgPanelH()) {
        IgCloseRect(&r);
        if (lx >= r.left && ly < r.bottom) {
            // Same contract as the Win32 WM_CLOSE: the raw view must never
            // outlive its only visible off-switch.
            InterlockedExchange(&g_aoRawView, 0);
            InterlockedExchange(&g_aoTweakOpen, 0);
            g_igPrevDown = down;
            return;
        }
        if (ly < IG_TITLE_H) {
            g_igDrag = 1;
            g_igDragOffX = lx; g_igDragOffY = ly;
            g_igPrevDown = down;
            return;
        }
        for (size_t i = 0; i < AO_ROWS; i++) {
            RECT t;
            IgRowTrack((int)i, &t);
            if (ly >= t.top - 4 && ly < t.bottom + 4 &&
                lx >= t.left - 4 && lx < t.right + 4) {
                g_igDrag = 2 + (int)i;
                g_igPrevDown = down;
                return;                              // value updates next tick
            }
        }
        IgCheckRect(&r);
        if (ly >= r.top && ly < r.bottom && lx < r.right) {
            InterlockedExchange(&g_aoRawView, !g_aoRawView);
            g_igPrevDown = down;
            return;
        }
        IgResRect(&r);
        if (ly >= r.top && ly < r.bottom && lx >= r.left && lx < r.right) {
            g_igDropOpen = 1;
            g_igPrevDown = down;
            return;
        }
        IgResetRect(&r);
        if (ly >= r.top && ly < r.bottom && lx >= r.left && lx < r.right) {
            DWORD now = GetTickCount();
            if (g_igResetArm && now - g_igResetArm < 3000) {
                g_igResetArm = 0;
                CfgResetDefaults(1);
            } else {
                g_igResetArm = now;
            }
        }
        g_igPrevDown = down;
        return;                                      // click consumed by the panel
    }

    // Outside the AO panel: the display surfaces, in the same order they are
    // drawn (topmost first) so an overlap resolves the way it looks.
    if (click) {
        if (g_statusEnabled &&
            mx >= g_statusX && mx < g_statusX + STAT_W &&
            my >= g_statusY && my < g_statusY + STAT_H) {
            g_igDrag = IG_DRAG_STAT;
            g_igDragOffX = mx - g_statusX; g_igDragOffY = my - g_statusY;
        } else if (g_overlayEnabled &&
                   mx >= g_overlayX && mx < g_overlayX + OVL_W &&
                   my >= g_overlayY && my < g_overlayY + OVL_H) {
            g_igDrag = IG_DRAG_OVL;
            g_igDragOffX = mx - g_overlayX; g_igDragOffY = my - g_overlayY;
        }
    }
    // The reset arm deliberately survives mouse-up - the 3s timeout is what
    // ends it, so the second click can be a normal separate click.
    g_igPrevDown = down;
}

// ---- D3D --------------------------------------------------------------------
static void IgReleaseGpu(void)
{
    if (g_igTex) { IDirect3DTexture9_Release(g_igTex); g_igTex = NULL; }
}

static int IgEnsureGpu(IDirect3DDevice9 *dev)
{
    if (g_igPsState == 0) {
        g_igPs = AoCompilePs(dev,
            "sampler2D t : register(s12);\n"
            "float4 main(float2 uv : TEXCOORD0) : COLOR { return tex2D(t, uv); }\n",
            NULL, "in-game panel blit");
        g_igPsState = g_igPs ? 1 : -1;
    }
    if (g_igPsState < 0) return 0;
    if (!g_igTex) {
        // MANAGED survives Reset by itself and is what a non-Ex device (the
        // shipping ForceStdD3D9=1 path) wants; an Ex device refuses MANAGED,
        // and there DEFAULT+DYNAMIC ALSO survives Reset (ResetEx keeps
        // resources). Either way no release-before-Reset plumbing is needed,
        // and a failed draw path recreates lazily as the last resort.
        if (FAILED(IDirect3DDevice9_CreateTexture(dev, IG_W, IG_TEX_H, 1, 0,
                       D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_igTex, NULL)) &&
            FAILED(IDirect3DDevice9_CreateTexture(dev, IG_W, IG_TEX_H, 1,
                       D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                       &g_igTex, NULL))) {
            g_igTex = NULL;
            return 0;
        }
    }
    return 1;
}

static int IgUpload(void)
{
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(g_igTex, 0, &lr, NULL, D3DLOCK_DISCARD)))
        if (FAILED(IDirect3DTexture9_LockRect(g_igTex, 0, &lr, NULL, 0)))
            return 0;
    for (int y = 0; y < IG_TEX_H; y++) {
        const unsigned int *src = (const unsigned int *)g_igBits + (size_t)y * IG_W;
        unsigned int *dst = (unsigned int *)((char *)lr.pBits + (size_t)y * lr.Pitch);
        // GDI leaves the alpha byte 0; the panel is opaque, so force it.
        for (int x = 0; x < IG_W; x++) dst[x] = src[x] | 0xFF000000u;
    }
    IDirect3DTexture9_UnlockRect(g_igTex, 0);
    return 1;
}

// ---- generic in-frame surfaces: the frametime graph and the status panel ---
// Both already draw themselves into an HDC (DrawOverlayGraph, DrawStatusPanel
// in 10_overlay.c), which is the whole reason porting them is cheap: point
// those same functions at a DIB instead of a window's DC and the pixels are
// identical. Nothing about their content or layout changes.
//
// They are DISPLAY surfaces - no widgets, no hit-testing - so they need none
// of the AO panel's interaction machinery, only position and (for the graph)
// the translucency its layered window used to provide.
typedef struct {
    int w, h;
    HDC dc;
    HBITMAP bmp;
    void *bits;
    IDirect3DTexture9 *tex;
} IgSurf;

static IgSurf g_igOvlSurf, g_igStatSurf;

static int IgSurfEnsure(IDirect3DDevice9 *dev, IgSurf *s, int w, int h)
{
    if (!s->dc) {
        BITMAPINFO bi;
        HDC screen = GetDC(NULL);
        if (!screen) return 0;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;          // top-down, matching the lock copy
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        s->dc = CreateCompatibleDC(screen);
        s->bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &s->bits, NULL, 0);
        ReleaseDC(NULL, screen);
        if (!s->dc || !s->bmp || !s->bits) return 0;
        SelectObject(s->dc, s->bmp);
        s->w = w; s->h = h;
    }
    if (!s->tex) {
        // Same pool reasoning as the AO panel's texture: MANAGED survives
        // Reset on the shipping non-Ex device, DEFAULT+DYNAMIC survives
        // ResetEx on an Ex one, so neither needs release-before-Reset
        // plumbing.
        if (FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, 0, D3DFMT_A8R8G8B8,
                                                  D3DPOOL_MANAGED, &s->tex, NULL)) &&
            FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, D3DUSAGE_DYNAMIC,
                                                  D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                  &s->tex, NULL))) {
            s->tex = NULL;
            return 0;
        }
    }
    return 1;
}

// alpha is the CONSTANT opacity for the whole surface - the graph's layered
// window used SetLayeredWindowAttributes(205) and this is how that look is
// reproduced now that there is no window to be layered.
static int IgSurfUpload(IgSurf *s, unsigned int alpha)
{
    D3DLOCKED_RECT lr;
    unsigned int a = (alpha & 0xFFu) << 24;
    if (FAILED(IDirect3DTexture9_LockRect(s->tex, 0, &lr, NULL, D3DLOCK_DISCARD)) &&
        FAILED(IDirect3DTexture9_LockRect(s->tex, 0, &lr, NULL, 0)))
        return 0;
    for (int y = 0; y < s->h; y++) {
        const unsigned int *src = (const unsigned int *)s->bits + (size_t)y * s->w;
        unsigned int *dst = (unsigned int *)((char *)lr.pBits + (size_t)y * lr.Pitch);
        for (int x = 0; x < s->w; x++) dst[x] = (src[x] & 0x00FFFFFFu) | a;
    }
    IDirect3DTexture9_UnlockRect(s->tex, 0);
    return 1;
}

// Auto-placement when no position has been chosen (-1), matching where the
// Win32 windows used to put themselves: graph bottom-left, status top-right,
// so the two never land on each other before either is moved.
static void IgAutoPos(LONG *px, LONG *py, int w, int h, int topRight)
{
    const int m = 20;
    LONG bw = (LONG)g_backbufW, bh = (LONG)g_backbufH;
    if (bw < w + 2 * m) bw = w + 2 * m;
    if (bh < h + 2 * m) bh = h + 2 * m;
    if (*px < 0) *px = topRight ? bw - w - m : m;
    if (*py < 0) *py = topRight ? m : bh - h - m;
    if (*px > bw - 40) *px = bw - w - m;
    if (*py > bh - 40) *py = bh - h - m;
    if (*px < 0) *px = 0;
    if (*py < 0) *py = 0;
}

static void IgQuad(IDirect3DDevice9 *dev, float x, float y, float w, float h,
                   float u0, float v0, float u1, float v1)
{
    // The -0.5 texel-centre snap, same constant the AO quads use - without it
    // the whole panel samples half a texel off and text goes soft.
    IgVtx v[4] = {
        { x - 0.5f,     y - 0.5f,     0, 1, u0, v0 },
        { x + w - 0.5f, y - 0.5f,     0, 1, u1, v0 },
        { x - 0.5f,     y + h - 0.5f, 0, 1, u0, v1 },
        { x + w - 0.5f, y + h - 0.5f, 0, 1, u1, v1 },
    };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(IgVtx));
}

// Called from HookedEndScene (13) - and nominally from the Present hooks,
// which have never fired in this game but might under another wrapper stack.
//
// SPLIT into prep and draw (2026-08-18, the ~20fps report): EndScene can run
// MANY times per frame if the engine brackets per pass, and the first version
// did EVERYTHING per call - GDI repaint, full texture upload, input polling,
// and (worst) the EnumWindows walk hiding in the window lookup. The
// per-frame work now latches on g_msFrameSeq and runs once; each EndScene
// pays only the draw itself (a handful of state sets and three quads), which
// is what makes "last bracket wins" affordable. The one-shot rate line below
// reports calls-per-frame so the cost model is measured, not assumed.
static volatile LONG g_igEsCalls = 0, g_igEsRateLogged = 0;

static void IgPresent(IDirect3DDevice9 *dev)
{
    static LONG lastPrepFrame = -1;
    static LONG haveMouseS = 0, mxS = 0, myS = 0;
    LONG mx = 0, my = 0;
    int haveMouse;

    int wantAo, wantOvl, wantStat;

    if (!dev || !g_inGameUi) return;
    wantAo   = (g_aoTweakOpen != 0);
    wantOvl  = (g_overlayEnabled != 0);
    wantStat = (g_statusEnabled != 0);
    if (!wantAo && !wantOvl && !wantStat) return;
    // EndScene rate, measured once: past ~600 frames, report how many times
    // this ran per frame. >1 means per-pass brackets and the draw runs that
    // many times; the prep never does.
    {
        LONG c = InterlockedIncrement(&g_igEsCalls);
        if (g_msFrameSeq > 600 && InterlockedCompareExchange(&g_igEsRateLogged, 1, 0) == 0) {
            char l[128];
            sprintf(l, "[menu] in-game UI: EndScene calls=%ld over %ld frames (~%ld per frame)",
                    c, g_msFrameSeq, g_msFrameSeq ? (c + g_msFrameSeq / 2) / g_msFrameSeq : 0);
            LogLine(l);
        }
    }
    // The open request is logged BEFORE the two ensures, and each ensure
    // failure logs itself. The first flight had the only log line AFTER both,
    // so whichever failed did so in silence - the same
    // absence-of-evidence trap as the language probe, fallen into the same
    // day. Every early return between "user asked" and "pixels drawn" now
    // leaves a trace.
    if (wantAo) {
        if (InterlockedCompareExchange(&g_igOpenLogged, 1, 0) == 0)
            LogLine("[menu] AO panel: open request reached the render hook");
        if (!IgEnsureGdi()) {
            static volatile LONG f = 0;
            if (InterlockedCompareExchange(&f, 1, 0) == 0)
                LogLine("[menu] AO panel: GDI setup FAILED (DIB or DC creation)");
            wantAo = 0;
        } else if (!IgEnsureGpu(dev)) {
            static volatile LONG f = 0;
            if (InterlockedCompareExchange(&f, 1, 0) == 0) {
                char l[128];
                sprintf(l, "[menu] AO panel: GPU setup FAILED (psState=%ld tex=%p)",
                        g_igPsState, (void *)g_igTex);
                LogLine(l);
            }
            wantAo = 0;
        }
    }
    // The blit shader is shared by all three surfaces; without the AO panel
    // open nothing else has created it yet.
    if ((wantOvl || wantStat) && !IgEnsureGpu(dev)) { wantOvl = wantStat = 0; }
    if (wantOvl && !IgSurfEnsure(dev, &g_igOvlSurf, OVL_W, OVL_H)) wantOvl = 0;
    if (wantStat && !IgSurfEnsure(dev, &g_igStatSurf, STAT_W, STAT_H)) wantStat = 0;
    if (!wantAo && !wantOvl && !wantStat) return;

    // ---- prep: once per engine frame, however many EndScenes it has -------
    if (g_msFrameSeq != lastPrepFrame) {
        lastPrepFrame = g_msFrameSeq;

        haveMouseS = IgCursor(&mxS, &myS);
        if (haveMouseS) IgInput(mxS, myS);

        if (wantAo && g_aoTweakOpen) {
            // Keep the panel reachable after a resolution change shrinks the screen.
            if (g_backbufW > IG_W && g_igX > (LONG)g_backbufW - 40) g_igX = (LONG)g_backbufW - IG_W;
            if (g_backbufH > IG_H && g_igY > (LONG)g_backbufH - 40) g_igY = (LONG)g_backbufH - IG_H;
            if (g_igX < 0) g_igX = 0;
            if (g_igY < 0) g_igY = 0;
            IgPaint(haveMouseS ? mxS - g_igX : -100, haveMouseS ? myS - g_igY : -100);
            if (!IgUpload()) { IgReleaseGpu(); wantAo = 0; }
        }
        // The two display surfaces: their own paint functions, unchanged,
        // pointed at a DIB instead of a window DC. The graph carries the 205
        // alpha its layered window used to apply.
        if (wantOvl) {
            IgAutoPos(&g_overlayX, &g_overlayY, OVL_W, OVL_H, 0);
            DrawOverlayGraph(g_igOvlSurf.dc);
            if (!IgSurfUpload(&g_igOvlSurf, 205)) wantOvl = 0;
        }
        if (wantStat) {
            IgAutoPos(&g_statusX, &g_statusY, STAT_W, STAT_H, 1);
            DrawStatusPanel(g_igStatSurf.dc);
            if (!IgSurfUpload(&g_igStatSurf, 255)) wantStat = 0;
        }
    }
    // Input may have closed the AO panel this tick.
    if (!g_aoTweakOpen) wantAo = 0;
    haveMouse = (int)haveMouseS;
    mx = mxS; my = myS;

    // ---- draw, with explicit save/restore ----------------------------------
    // Restore comes from the engine-state shadows (15) exactly like the AO
    // bracket: restore ONLY what was touched, through g_orig*, no state
    // blocks. The render target and depth-stencil are saved through the REAL
    // Get methods - the hooked ones lie while MSAA substitutes, and what must
    // go back is whatever is genuinely bound right now.
    {
        IDirect3DSurface9 *oldRt = NULL, *oldDs = NULL, *bb = NULL;
        IDirect3DPixelShader9 *oldPs = NULL;
        if (g_origGetRenderTarget) g_origGetRenderTarget(dev, 0, &oldRt);
        if (g_origGetDepthStencil) g_origGetDepthStencil(dev, &oldDs);
        IDirect3DDevice9_GetPixelShader(dev, &oldPs);

        if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0,
                          D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            g_origSetRT(dev, 0, bb);
            g_origSetDS(dev, NULL);   // no depth: mismatched sample counts can
                                      // reject the draw, and the panel needs none

            g_origSetRenderState(dev, D3DRS_ZENABLE, FALSE);
            g_origSetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
            // Blending ON for the whole bracket: the frametime graph carries
            // a constant 205 alpha, which is how its layered window's
            // translucency is reproduced without a window. The opaque
            // surfaces upload alpha 255 and are unaffected.
            g_origSetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
            g_origSetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            g_origSetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            g_origSetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
            g_origSetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
            g_origSetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
            g_origSetRenderState(dev, D3DRS_COLORWRITEENABLE, 0x0F);
            g_origSetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
            g_origSetRenderState(dev, D3DRS_FOGENABLE, FALSE);

            g_origSetVertexShader(dev, NULL);        // XYZRHW needs the FF path
            g_origSetPixelShader(dev, g_igPs);
            g_origSetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
            IDirect3DDevice9_SetSamplerState(dev, 12, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            IDirect3DDevice9_SetSamplerState(dev, 12, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            IDirect3DDevice9_SetSamplerState(dev, 12, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            IDirect3DDevice9_SetSamplerState(dev, 12, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

            // Painter's order = Z-order: displays first, the interactive panel
            // over them, the cursor last so it is never occluded.
            if (wantOvl) {
                g_origSetTexture(dev, 12, (IDirect3DBaseTexture9 *)g_igOvlSurf.tex);
                IgQuad(dev, (float)g_overlayX, (float)g_overlayY,
                       (float)OVL_W, (float)OVL_H, 0.0f, 0.0f, 1.0f, 1.0f);
            }
            if (wantStat) {
                g_origSetTexture(dev, 12, (IDirect3DBaseTexture9 *)g_igStatSurf.tex);
                IgQuad(dev, (float)g_statusX, (float)g_statusY,
                       (float)STAT_W, (float)STAT_H, 0.0f, 0.0f, 1.0f, 1.0f);
            }
            if (wantAo) {
                // Only the LIVE height: the texture is always allocated tall
                // enough for an open dropdown, but the quad shows just the part
                // that is currently painted, so a closed panel has no dead strip
                // hanging off its bottom.
                float ph = (float)IgPanelH();
                g_origSetTexture(dev, 12, (IDirect3DBaseTexture9 *)g_igTex);
                IgQuad(dev, (float)g_igX, (float)g_igY, (float)IG_W, ph,
                       0.0f, 0.0f, 1.0f, ph / (float)IG_TEX_H);
            }
            // Cursor: crosshair from the white patch. Only while the panel that
            // owns that patch is up - and only when the mapping succeeded, since
            // a wrong cursor is worse than none.
            if (haveMouse && wantAo) {
                float wu = (IG_WHITE_X + 4.0f) / IG_W, wv = (IG_WHITE_Y + 4.0f) / IG_TEX_H;
                g_origSetTexture(dev, 12, (IDirect3DBaseTexture9 *)g_igTex);
                IgQuad(dev, (float)mx - 7, (float)my - 1, 14, 2, wu, wv, wu, wv);
                IgQuad(dev, (float)mx - 1, (float)my - 7, 2, 14, wu, wv, wu, wv);
            }

            // ---- restore ---------------------------------------------------
            g_origSetRT(dev, 0, oldRt);              // resets viewport to RT size
            g_origSetDS(dev, oldDs);
            if (g_esVpKnown) g_origSetViewport(dev, &g_esVp);
            g_origSetPixelShader(dev, oldPs);
            g_origSetVertexShader(dev, (IDirect3DVertexShader9 *)g_esVs);
            if (g_esDeclIsFvf) g_origSetFVF(dev, g_esFvf);
            else g_origSetVertexDecl(dev, (IDirect3DVertexDeclaration9 *)g_esDecl);
            g_origSetStreamSource(dev, 0, (IDirect3DVertexBuffer9 *)g_esStreamVb,
                                  g_esStreamOffset, g_esStreamStride);
            g_origSetTexture(dev, 12, (IDirect3DBaseTexture9 *)g_esTex[12]);
            g_origSetRenderState(dev, D3DRS_ZENABLE, EsRs(D3DRS_ZENABLE, TRUE));
            g_origSetRenderState(dev, D3DRS_ZWRITEENABLE, EsRs(D3DRS_ZWRITEENABLE, TRUE));
            g_origSetRenderState(dev, D3DRS_ALPHABLENDENABLE, EsRs(D3DRS_ALPHABLENDENABLE, FALSE));
            // The two factors are restored because the bracket now SETS them
            // for the graph's translucency - restore only what you touched,
            // but restore all of it.
            g_origSetRenderState(dev, D3DRS_SRCBLEND, EsRs(D3DRS_SRCBLEND, D3DBLEND_ONE));
            g_origSetRenderState(dev, D3DRS_DESTBLEND, EsRs(D3DRS_DESTBLEND, D3DBLEND_ZERO));
            g_origSetRenderState(dev, D3DRS_ALPHATESTENABLE, EsRs(D3DRS_ALPHATESTENABLE, FALSE));
            g_origSetRenderState(dev, D3DRS_STENCILENABLE, EsRs(D3DRS_STENCILENABLE, FALSE));
            g_origSetRenderState(dev, D3DRS_CULLMODE, EsRs(D3DRS_CULLMODE, D3DCULL_CCW));
            g_origSetRenderState(dev, D3DRS_COLORWRITEENABLE, EsRs(D3DRS_COLORWRITEENABLE, 0x0F));
            g_origSetRenderState(dev, D3DRS_SCISSORTESTENABLE, EsRs(D3DRS_SCISSORTESTENABLE, FALSE));
            g_origSetRenderState(dev, D3DRS_FOGENABLE, EsRs(D3DRS_FOGENABLE, FALSE));
            IDirect3DSurface9_Release(bb);
        }
        if (oldRt) IDirect3DSurface9_Release(oldRt);
        if (oldDs) IDirect3DSurface9_Release(oldDs);
        if (oldPs) IDirect3DPixelShader9_Release(oldPs);
    }
}

#else
static void IgPresent(IDirect3DDevice9 *dev) { (void)dev; }
#endif  // ENABLE_AO_SSAO
