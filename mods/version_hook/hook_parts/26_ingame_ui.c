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
// rows + raw checkbox + resolution button + reset button + bottom pad
#define IG_H (IG_TITLE_H + IG_PAD + (int)AO_ROWS * IG_ROW_H + 22 + 26 + 26 + IG_PAD)
// The white patch the cursor quads sample: bottom-right corner, drawn last so
// nothing paints over it. 8x8 so bilinear can never bleed an edge in.
#define IG_WHITE_X  (IG_W - 8)
#define IG_WHITE_Y  (IG_H - 8)

// (g_aoPanelInGame is declared in 01_config_gates.c - the config table in 08
// and the Win32 gate in 10 both sit earlier in the TU and need it.)

// Panel position in backbuffer pixels. Session-only on purpose for now: the
// backbuffer size can differ between sessions (resolution change), and a
// stale persisted position could park the panel off-screen with no window
// manager to drag it back. Re-centre-ish every boot instead.
static LONG g_igX = 80, g_igY = 80;

// Interaction state, render thread only (IgPresent is the sole writer).
static int  g_igPrevDown = 0;
static int  g_igDrag = 0;          // 0 none, 1 = title bar, 2+i = slider row i
static LONG g_igDragOffX = 0, g_igDragOffY = 0;
static DWORD g_igResetArm = 0;     // tick count of first reset click, 0 = disarmed
static LONG g_igOpenLogged = 0;

// GDI side.
static HDC     g_igDc = NULL;
static HBITMAP g_igBmp = NULL;
static void   *g_igBits = NULL;    // 32bpp top-down, IG_W x IG_H
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
static void IgResRect(RECT *r)
{
    r->left = IG_PAD + IG_LABEL_W;
    r->top = IG_TITLE_H + IG_PAD + (int)AO_ROWS * IG_ROW_H + 24;
    r->right = r->left + IG_TRACK_W + 8 + IG_VAL_W; r->bottom = r->top + 22;
}
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
    bi.bmiHeader.biHeight = -IG_H;          // top-down, so rows match the lock copy
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
    r.left = 0; r.top = 0; r.right = IG_W; r.bottom = IG_H;
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

    // Resolution: one button cycling Native/Half/Quarter. A dropdown needs a
    // popup and a popup is a WINDOW - the entire species this panel exists to
    // escape. Cycling through three values costs at most two clicks.
    IgResRect(&r);
    IgFill(&r, (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom)
                   ? RGB(66, 72, 84) : RGB(52, 56, 64));
    {
        int idx = AoResIndexOf(g_aoResDiv);
        IgText(r.left, r.top + 1, r.right - r.left,
               TR(idx == 0 ? S_RES_NATIVE : idx == 1 ? S_RES_HALF : S_RES_QUARTER),
               RGB(225, 225, 225), g_igFont, DT_CENTER);
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

    // The cursor's white patch, painted last so nothing covers it.
    r.left = IG_WHITE_X; r.top = IG_WHITE_Y; r.right = IG_W; r.bottom = IG_H;
    IgFill(&r, RGB(255, 255, 255));
}

// ---- input ------------------------------------------------------------------
// Backbuffer-space cursor. Client coords scale by backbuffer/client because
// borderless keeps the backbuffer at desktop size while the client can be
// anything; at 1:1 the scale is identity and this is a no-op.
static int IgCursor(LONG *ox, LONG *oy)
{
    POINT p;
    RECT rc;
    HWND w = g_gameHwnd ? g_gameHwnd : GameMenuFindWindow();
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

    if (click && lx >= 0 && lx < IG_W && ly >= 0 && ly < IG_H) {
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
            int idx = (AoResIndexOf(g_aoResDiv) + 1) % 3;
            InterlockedExchange(&g_aoResDiv, g_aoResDivs[idx]);
            SaveConfig();
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
        if (FAILED(IDirect3DDevice9_CreateTexture(dev, IG_W, IG_H, 1, 0,
                       D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_igTex, NULL)) &&
            FAILED(IDirect3DDevice9_CreateTexture(dev, IG_W, IG_H, 1,
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
    for (int y = 0; y < IG_H; y++) {
        const unsigned int *src = (const unsigned int *)g_igBits + (size_t)y * IG_W;
        unsigned int *dst = (unsigned int *)((char *)lr.pBits + (size_t)y * lr.Pitch);
        // GDI leaves the alpha byte 0; the panel is opaque, so force it.
        for (int x = 0; x < IG_W; x++) dst[x] = src[x] | 0xFF000000u;
    }
    IDirect3DTexture9_UnlockRect(g_igTex, 0);
    return 1;
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

// Called from both Present hooks in 13, before the engine's frame goes out.
static void IgPresent(IDirect3DDevice9 *dev)
{
    LONG mx = 0, my = 0;
    int haveMouse;

    if (!g_aoTweakOpen || !g_aoPanelInGame || !dev) return;
    // The open request is logged BEFORE the two ensures, and each ensure
    // failure logs itself. The first flight had the only log line AFTER both,
    // so whichever failed did so in silence - the same
    // absence-of-evidence trap as the language probe, fallen into the same
    // day. Every early return between "user asked" and "pixels drawn" now
    // leaves a trace.
    if (InterlockedCompareExchange(&g_igOpenLogged, 1, 0) == 0)
        LogLine("[menu] AO panel: open request reached the Present hook");
    if (!IgEnsureGdi()) {
        static volatile LONG f = 0;
        if (InterlockedCompareExchange(&f, 1, 0) == 0)
            LogLine("[menu] AO panel: GDI setup FAILED (DIB or DC creation)");
        return;
    }
    if (!IgEnsureGpu(dev)) {
        static volatile LONG f = 0;
        if (InterlockedCompareExchange(&f, 1, 0) == 0) {
            char l[128];
            sprintf(l, "[menu] AO panel: GPU setup FAILED (psState=%ld tex=%p)",
                    g_igPsState, (void *)g_igTex);
            LogLine(l);
        }
        return;
    }

    haveMouse = IgCursor(&mx, &my);
    if (haveMouse) IgInput(mx, my);
    // Input may have closed the panel this very tick.
    if (!g_aoTweakOpen) return;

    // Keep the panel reachable after a resolution change shrinks the screen.
    if (g_backbufW > IG_W && g_igX > (LONG)g_backbufW - 40) g_igX = (LONG)g_backbufW - IG_W;
    if (g_backbufH > IG_H && g_igY > (LONG)g_backbufH - 40) g_igY = (LONG)g_backbufH - IG_H;
    if (g_igX < 0) g_igX = 0;
    if (g_igY < 0) g_igY = 0;

    IgPaint(haveMouse ? mx - g_igX : -100, haveMouse ? my - g_igY : -100);
    if (!IgUpload()) { IgReleaseGpu(); return; }

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
            g_origSetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
            g_origSetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
            g_origSetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
            g_origSetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
            g_origSetRenderState(dev, D3DRS_COLORWRITEENABLE, 0x0F);
            g_origSetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
            g_origSetRenderState(dev, D3DRS_FOGENABLE, FALSE);

            g_origSetVertexShader(dev, NULL);        // XYZRHW needs the FF path
            g_origSetPixelShader(dev, g_igPs);
            g_origSetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
            g_origSetTexture(dev, 12, (IDirect3DBaseTexture9 *)g_igTex);
            IDirect3DDevice9_SetSamplerState(dev, 12, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            IDirect3DDevice9_SetSamplerState(dev, 12, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            IDirect3DDevice9_SetSamplerState(dev, 12, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            IDirect3DDevice9_SetSamplerState(dev, 12, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

            IgQuad(dev, (float)g_igX, (float)g_igY, (float)IG_W, (float)IG_H,
                   0.0f, 0.0f, 1.0f, 1.0f);
            // Cursor: crosshair from the white patch. Drawn only when the
            // mapping succeeded - a wrong cursor is worse than none.
            if (haveMouse) {
                float wu = (IG_WHITE_X + 4.0f) / IG_W, wv = (IG_WHITE_Y + 4.0f) / IG_H;
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
