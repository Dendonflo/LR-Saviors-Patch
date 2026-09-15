// ---- In-game shadow tuning panel (2026-09-14) -------------------------------
// Second interactive in-frame panel, built on the AO panel's machinery in 26:
// same DIB->texture->one-quad path, same polled input, same "restore only
// what you touched" draw bracket (it is drawn INSIDE IgPresent's bracket, not
// in one of its own). The AO panel's IgFill/IgText paint through g_igPaintDc,
// which this file points at its own DIB while painting.
//
// Content follows the Shadow Softness menu: a three-way mode selector
// (Off / On / PCSS) mirroring what the menu shows, then the rows that mode
// has - On: one Softness % slider (100 = vanilla-Advanced softness at any
// ShadowMapRes); PCSS: light size, min/max radius, search radius, bias -
// and a double-click reset that restores only the SELECTED mode's values:
// On -> Softness back to 100, PCSS -> the five PCSS keys to their defaults,
// Off -> nothing. The mode itself and everything else are left alone. The
// projection mode is ini-only (ShadowProjMode).
//
// Height depends on the mode; the texture is allocated for the tallest.
#if ENABLE_AO_SSAO && ENABLE_SHADOW_PCSS

#define SP_W        428
#define SP_MODE_H   26          // the segmented selector row
#define SP_MAX_ROWS 5
#define SP_H_FOR(rows) (IG_TITLE_H + IG_PAD + SP_MODE_H + 6 + (rows) * IG_ROW_H + 28 + IG_PAD)
#define SP_TEX_H    (SP_H_FOR(SP_MAX_ROWS) + 12)   // + the cursor's white patch strip

static LONG g_spX = 120, g_spY = 120;
static int  g_spDrag = 0;                // 1 title, 2+i slider i
static LONG g_spDragOffX = 0, g_spDragOffY = 0;
static DWORD g_spResetArm = 0;
static IgSurf g_spSurf;
static int  g_spPrevDown = 0;

typedef struct { int nameId; volatile LONG *val; LONG lo, hi; } SpRow;
static const SpRow g_spSoftRows[] = {
    { S_SOFTNESS,      &g_shadowFilterPct,   25,  400 },
};
static const SpRow g_spPcssRows[] = {
    { S_LIGHT_SIZE,    &g_pcssLightSize,      0,  200 },
    { S_MIN_RADIUS,    &g_pcssMinRadius,      0,  100 },
    { S_MAX_RADIUS,    &g_pcssMaxRadius,      1,   64 },
    { S_SEARCH_RADIUS, &g_pcssSearchRadius,   1,   64 },
    { S_BIAS,          &g_pcssBias,           0, 2000 },
};

// 0 off, 1 on (percentage), 2 PCSS - derived from the live settings, so the
// panel always shows what the menu would.
static int SpMode(void)
{
    if (g_shadowPcss) return 2;
    return g_shadowFilterPct > 0 ? 1 : 0;
}
static const SpRow *SpRows(int *count)
{
    int m = SpMode();
    if (m == 1) { *count = (int)(sizeof(g_spSoftRows) / sizeof(g_spSoftRows[0])); return g_spSoftRows; }
    if (m == 2) { *count = (int)(sizeof(g_spPcssRows) / sizeof(g_spPcssRows[0])); return g_spPcssRows; }
    *count = 0; return NULL;
}
static int SpPanelH(void) { int n; SpRows(&n); return SP_H_FOR(n); }

static void SpSetMode(int m)
{
    ShadowSoftSetMode(m);            // 09: remembered percentage, shared with the menu
    GameMenuRefreshChecks();
}

// ---- layout ----
static void SpModeRect(int seg, RECT *r)
{
    int w = (SP_W - 2 * IG_PAD) / 3;
    r->left = IG_PAD + seg * w; r->right = r->left + w - 2;
    r->top = IG_TITLE_H + IG_PAD; r->bottom = r->top + SP_MODE_H - 4;
}
static void SpRowTrack(int i, RECT *r)
{
    r->left = IG_PAD + IG_LABEL_W;
    r->top = IG_TITLE_H + IG_PAD + SP_MODE_H + 6 + i * IG_ROW_H + 8;
    r->right = r->left + IG_TRACK_W;
    r->bottom = r->top + 16;
}
static void SpResetRect(RECT *r)
{
    int n; SpRows(&n);
    r->left = IG_PAD + IG_LABEL_W;
    r->top = IG_TITLE_H + IG_PAD + SP_MODE_H + 6 + n * IG_ROW_H + 4;
    r->right = r->left + IG_TRACK_W + 8 + IG_VAL_W; r->bottom = r->top + 22;
}
static void SpCloseRect(RECT *r)
{
    r->left = SP_W - IG_TITLE_H; r->top = 0; r->right = SP_W; r->bottom = IG_TITLE_H;
}

// ---- paint ----
static void SpPaint(int mx, int my)
{
    RECT r;
    wchar_t buf[64];
    int n, mode = SpMode();
    const SpRow *rows = SpRows(&n);
    static const int segIds[3] = { S_OFF, S_ON, -1 };

    g_igPaintDc = g_spSurf.dc;
    SetBkMode(g_igPaintDc, TRANSPARENT);
    r.left = 0; r.top = 0; r.right = SP_W; r.bottom = SpPanelH();
    IgFill(&r, RGB(28, 30, 34));
    r.bottom = IG_TITLE_H;
    IgFill(&r, RGB(46, 50, 58));
    IgText(IG_PAD, 2, SP_W - IG_TITLE_H - IG_PAD, TR(S_SHADOW_TUNING),
           RGB(235, 235, 235), g_igFontBold, DT_LEFT);
    SpCloseRect(&r);
    if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom)
        IgFill(&r, RGB(140, 40, 40));
    IgText(r.left, 2, r.right - r.left, L"\x2715", RGB(220, 220, 220), g_igFont, DT_CENTER);

    // Mode selector: three segments, the active one lit.
    for (int s = 0; s < 3; s++) {
        int hot;
        SpModeRect(s, &r);
        hot = (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom);
        IgFill(&r, (s == mode) ? RGB(58, 108, 178) : hot ? RGB(66, 72, 84) : RGB(52, 56, 64));
        IgText(r.left, r.top + 1, r.right - r.left, segIds[s] >= 0 ? TR(segIds[s]) : L"PCSS",
               (s == mode) ? RGB(255, 255, 255) : RGB(210, 210, 210),
               (s == mode) ? g_igFontBold : g_igFont, DT_CENTER);
    }

    for (int i = 0; i < n; i++) {
        RECT t;
        LONG v = *rows[i].val, lo = rows[i].lo, hi = rows[i].hi;
        SpRowTrack(i, &t);
        IgText(IG_PAD, t.top - 4, IG_LABEL_W - 4, TR(rows[i].nameId),
               RGB(210, 210, 210), g_igFont, DT_LEFT);
        r = t; r.top += 5; r.bottom -= 5;
        IgFill(&r, RGB(58, 62, 70));
        if (hi > lo) {
            int cx = t.left + (int)((__int64)(v - lo) * (IG_TRACK_W - 10) / (hi - lo)) + 5;
            r.left = cx - 5; r.right = cx + 5; r.top = t.top; r.bottom = t.bottom;
            IgFill(&r, (g_spDrag == 2 + i) ? RGB(120, 170, 255) : RGB(150, 150, 160));
        }
        _snwprintf(buf, 64, L"%ld", v);
        buf[63] = 0;
        IgText(t.right + 8, t.top - 4, IG_VAL_W, buf, RGB(235, 235, 235), g_igFont, DT_RIGHT);
    }

    SpResetRect(&r);
    {
        int armed = g_spResetArm && (GetTickCount() - g_spResetArm < 3000);
        IgFill(&r, armed ? RGB(150, 60, 40)
                         : (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom)
                               ? RGB(66, 72, 84) : RGB(52, 56, 64));
        if (armed) { _snwprintf(buf, 64, L"%s ?", TR(S_RESET_SHADOW)); buf[63] = 0; }
        IgText(r.left, r.top + 1, r.right - r.left, armed ? buf : TR(S_RESET_SHADOW),
               RGB(225, 225, 225), g_igFont, DT_CENTER);
    }
    // Cursor source: a white patch below the tallest drawn height, so it
    // is never covered and never shown as part of the panel.
    r.left = SP_W - 8; r.top = SP_TEX_H - 8; r.right = SP_W; r.bottom = SP_TEX_H;
    IgFill(&r, RGB(255, 255, 255));
    g_igPaintDc = g_igDc;
}
static void SpCursorTex(IDirect3DBaseTexture9 **tex, float *wu, float *wv)
{
    *tex = (IDirect3DBaseTexture9 *)g_spSurf.tex;
    *wu = (SP_W - 4.0f) / SP_W;
    *wv = (SP_TEX_H - 4.0f) / SP_TEX_H;
}

// ---- input: returns 1 when the click/drag was consumed ----
static int SpInput(LONG mx, LONG my)
{
    int down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    int click = down && !g_spPrevDown;
    LONG lx = mx - g_spX, ly = my - g_spY;
    RECT r;
    int n;
    const SpRow *rows = SpRows(&n);

    if (g_spDrag == 1) {
        g_spX = mx - g_spDragOffX; g_spY = my - g_spDragOffY;
        if (!down) g_spDrag = 0;
        g_spPrevDown = down;
        return 1;
    }
    if (g_spDrag >= 2) {
        int i = g_spDrag - 2;
        if (i < n) {
            RECT t;
            SpRowTrack(i, &t);
            LONG lo = rows[i].lo, hi = rows[i].hi;
            LONG v = lo + (LONG)((__int64)(lx - t.left) * (hi - lo) / (IG_TRACK_W - 1));
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            InterlockedExchange(rows[i].val, v);
        }
        if (!down) {
            g_spDrag = 0;
            if (g_shadowFilterPct > 0) InterlockedExchange(&g_shadowFilterMem, g_shadowFilterPct);
            SaveConfig();
        }
        g_spPrevDown = down;
        return 1;
    }
    if (click && g_pcssTweakOpen &&
        lx >= 0 && lx < SP_W && ly >= 0 && ly < SpPanelH()) {
        SpCloseRect(&r);
        if (lx >= r.left && ly < r.bottom) {
            InterlockedExchange(&g_pcssTweakOpen, 0);
            g_spPrevDown = down;
            return 1;
        }
        if (ly < IG_TITLE_H) {
            g_spDrag = 1; g_spDragOffX = lx; g_spDragOffY = ly;
            g_spPrevDown = down;
            return 1;
        }
        for (int s = 0; s < 3; s++) {
            SpModeRect(s, &r);
            if (lx >= r.left && lx < r.right && ly >= r.top && ly < r.bottom) {
                if (s != SpMode()) SpSetMode(s);
                g_spPrevDown = down;
                return 1;
            }
        }
        for (int i = 0; i < n; i++) {
            RECT t;
            SpRowTrack(i, &t);
            if (ly >= t.top - 4 && ly < t.bottom + 4 && lx >= t.left - 4 && lx < t.right + 4) {
                g_spDrag = 2 + i;
                g_spPrevDown = down;
                return 1;
            }
        }
        SpResetRect(&r);
        if (ly >= r.top && ly < r.bottom && lx >= r.left && lx < r.right) {
            DWORD now = GetTickCount();
            if (g_spResetArm && now - g_spResetArm < 3000) {
                static const char *const pcssKeys[] = {
                    "PcssLightSize", "PcssMinRadiusX10", "PcssMaxRadius", "PcssSearchRadius", "PcssBias",
                };
                int m = SpMode();
                g_spResetArm = 0;
                if (m == 2) {
                    CfgResetKeys(pcssKeys, sizeof(pcssKeys) / sizeof(pcssKeys[0]));
                    LogLine("[config] shadow panel: PCSS values reset");
                } else if (m == 1) {
                    InterlockedExchange(&g_shadowFilterPct, 100);
                    SaveConfig();
                    LogLine("[config] shadow panel: softness reset to 100");
                }
            } else {
                g_spResetArm = now;
            }
        }
        g_spPrevDown = down;
        return 1;
    }
    g_spPrevDown = down;
    return 0;
}

// Per-frame prep (paint + upload), called from IgPresent's prep block. Returns
// 1 when the surface is ready to be drawn.
static int SpPrep(IDirect3DDevice9 *dev, int haveMouse, LONG mx, LONG my)
{
    if (!g_pcssTweakOpen) return 0;
    if (!IgSurfEnsure(dev, &g_spSurf, SP_W, SP_TEX_H)) return 0;
    if (g_backbufW > SP_W && g_spX > (LONG)g_backbufW - 40) g_spX = (LONG)g_backbufW - SP_W;
    if (g_backbufH > SP_TEX_H && g_spY > (LONG)g_backbufH - 40) g_spY = (LONG)g_backbufH - SP_TEX_H;
    if (g_spX < 0) g_spX = 0;
    if (g_spY < 0) g_spY = 0;
    SpPaint(haveMouse ? mx - g_spX : -100, haveMouse ? my - g_spY : -100);
    return IgSurfUpload(&g_spSurf);
}

// Draw, inside IgPresent's bracket (state already set, sampler 12, blit PS).
static void SpDraw(IDirect3DDevice9 *dev)
{
    float ph = (float)SpPanelH();
    float a[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 223, a, 1);
    g_origSetTexture(dev, 12, (IDirect3DBaseTexture9 *)g_spSurf.tex);
    IgQuad(dev, (float)g_spX, (float)g_spY, (float)SP_W, ph, 0.0f, 0.0f, 1.0f, ph / (float)SP_TEX_H);
}
#endif  // ENABLE_AO_SSAO && ENABLE_SHADOW_PCSS
