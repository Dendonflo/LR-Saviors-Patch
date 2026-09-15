// ---- PCSS tuning window (Win32, scrollbars) --------------------------------
// Same shape as the AO tuning window above: owned by the game window,
// MA_NOACTIVATE, scrollbars writing the live tunables (the PCSS bind uploads
// them every frame, so every notch is visible immediately), SaveConfig on
// release so the ini keeps whatever the user lands on. Opened from
// Graphics > Shadow Softness > Tuning Panel; closing hides it.
//
// Units are the ini's: light size = tan(half sun angle) x 1000; radii in
// 2048-map texels (min x10); bias x1e-5 depth units. Labels say so.
#if ENABLE_SHADOW_PCSS
// g_pcssTweakOpen is defined in 03_render_state.c (the menu handler in 09 needs it).
static HWND g_hPcssTweak = NULL;
static HWND g_hPcssOnCheck = NULL;
#define PTW_ON_ID   4101
#define PTW_ROW_H   34
#define PTW_LABEL_W 150
#define PTW_BAR_W   230
#define PTW_VAL_W   60

static struct {
    const wchar_t *name;
    volatile LONG *val;
    LONG lo, hi, step;
    HWND bar;
} g_pcssRows[] = {
    { L"Light size (x1000)",     &g_pcssLightSize,    0,  300, 1, NULL },
    { L"Min radius (texels x10)",&g_pcssMinRadius,    0,  100, 1, NULL },
    { L"Max radius (texels)",    &g_pcssMaxRadius,    1,   64, 1, NULL },
    { L"Search radius (texels)", &g_pcssSearchRadius, 1,   64, 1, NULL },
    { L"Depth bias (x1e-5)",     &g_pcssBias,         0, 2000, 5, NULL },
};
#define PCSS_ROWS (sizeof(g_pcssRows) / sizeof(g_pcssRows[0]))

static LRESULT CALLBACK PcssTweakProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_HSCROLL: {
        HWND bar = (HWND)lp;
        for (size_t i = 0; i < PCSS_ROWS; i++) {
            if (g_pcssRows[i].bar != bar) continue;
            LONG v = *g_pcssRows[i].val;
            int code = LOWORD(wp);
            if (code == SB_THUMBTRACK || code == SB_THUMBPOSITION) v = (LONG)HIWORD(wp);
            else if (code == SB_LINELEFT)  v -= g_pcssRows[i].step;
            else if (code == SB_LINERIGHT) v += g_pcssRows[i].step;
            else if (code == SB_PAGELEFT)  v -= g_pcssRows[i].step * 4;
            else if (code == SB_PAGERIGHT) v += g_pcssRows[i].step * 4;
            else if (code == SB_ENDSCROLL) { SaveConfig(); return 0; }
            if (v < g_pcssRows[i].lo) v = g_pcssRows[i].lo;
            if (v > g_pcssRows[i].hi) v = g_pcssRows[i].hi;
            InterlockedExchange(g_pcssRows[i].val, v);
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
        wchar_t t[64];
        for (size_t i = 0; i < PCSS_ROWS; i++) {
            int y = 10 + (int)i * PTW_ROW_H;
            TextOutW(dc, 10, y + 4, g_pcssRows[i].name, (int)wcslen(g_pcssRows[i].name));
            _snwprintf(t, 64, L"%ld", *g_pcssRows[i].val);
            t[63] = 0;
            TextOutW(dc, 10 + PTW_LABEL_W + PTW_BAR_W + 8, y + 4, t, (int)wcslen(t));
        }
        {
            // Live readout: what the shader actually receives after the
            // resolution scaling, so a value can be quoted back exactly.
            _snwprintf(t, 64, L"binds=%ld  state=%s", g_pcssBinds,
                       g_pcssState > 0 ? L"ok" : g_pcssState < 0 ? L"COMPILE FAILED" : L"idle");
            t[63] = 0;
            TextOutW(dc, 10, 10 + (int)PCSS_ROWS * PTW_ROW_H + 28, t, (int)wcslen(t));
        }
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == PTW_ON_ID && HIWORD(wp) == BN_CLICKED) {
            LONG on = (SendMessageA(g_hPcssOnCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
            InterlockedExchange(&g_shadowPcss, on);
            if (on) InterlockedExchange(&g_shadowFilterPct, 0);
            SaveConfig();
            return 0;
        }
        break;
    case WM_CLOSE:
        InterlockedExchange(&g_pcssTweakOpen, 0);
        ShowWindow(h, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void EnsurePcssTweakWindow(void)
{
    if (g_hPcssTweak) return;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = PcssTweakProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"LRSaviorPcssTweak";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    int cw = 10 + PTW_LABEL_W + PTW_BAR_W + 8 + PTW_VAL_W + 10;
    int ch = 20 + (int)PCSS_ROWS * PTW_ROW_H + 24 + 28;
    RECT r = { 0, 0, cw, ch };
    AdjustWindowRectEx(&r, WS_CAPTION | WS_SYSMENU | WS_POPUP, FALSE, WS_EX_TOOLWINDOW);
    HWND owner = g_gameHwnd ? g_gameHwnd : GameMenuFindWindow();
    g_hPcssTweak = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"PCSS Tuning",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        160, 160, r.right - r.left, r.bottom - r.top,
        owner, NULL, wc.hInstance, NULL);
    if (!g_hPcssTweak) return;
    for (size_t i = 0; i < PCSS_ROWS; i++) {
        int y = 10 + (int)i * PTW_ROW_H;
        g_pcssRows[i].bar = CreateWindowExW(
            0, L"SCROLLBAR", NULL, WS_CHILD | WS_VISIBLE | SBS_HORZ,
            10 + PTW_LABEL_W, y, PTW_BAR_W, 18,
            g_hPcssTweak, NULL, wc.hInstance, NULL);
        SCROLLINFO si;
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_POS | SIF_PAGE;
        si.nMin = (int)g_pcssRows[i].lo;
        si.nMax = (int)g_pcssRows[i].hi;
        si.nPos = (int)*g_pcssRows[i].val;
        si.nPage = 1;
        SetScrollInfo(g_pcssRows[i].bar, SB_CTL, &si, TRUE);
    }
    g_hPcssOnCheck = CreateWindowExW(
        0, L"BUTTON", L"PCSS enabled (off = engine shader)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        10, 10 + (int)PCSS_ROWS * PTW_ROW_H + 2, cw - 20, 20,
        g_hPcssTweak, (HMENU)(UINT_PTR)PTW_ON_ID, wc.hInstance, NULL);
    if (g_hPcssOnCheck)
        SendMessageA(g_hPcssOnCheck, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

// Polled from OverlayThread next to the AO window.
static void PcssTweakPoll(void)
{
    // In-frame mode (26b_shadow_panel.c) owns the panel; this window is the
    // InGameUi=0 fallback, exactly like the AO window above it.
    if (g_inGameUi) {
        if (g_hPcssTweak && IsWindowVisible(g_hPcssTweak)) ShowWindow(g_hPcssTweak, SW_HIDE);
        return;
    }
    if (g_pcssTweakOpen) {
        EnsurePcssTweakWindow();
        if (g_hPcssTweak && !IsWindowVisible(g_hPcssTweak)) {
            for (size_t i = 0; i < PCSS_ROWS; i++)
                if (g_pcssRows[i].bar)
                    SetScrollPos(g_pcssRows[i].bar, SB_CTL, (int)*g_pcssRows[i].val, TRUE);
            ShowWindow(g_hPcssTweak, SW_SHOWNOACTIVATE);
        }
        if (g_hPcssTweak && IsWindowVisible(g_hPcssTweak)) {
            if (g_hPcssOnCheck)
                SendMessageA(g_hPcssOnCheck, BM_SETCHECK,
                             g_shadowPcss ? BST_CHECKED : BST_UNCHECKED, 0);
            // Readout refresh about once a second; every tick would flicker.
            static DWORD lastPaint = 0;
            DWORD now = GetTickCount();
            if (now - lastPaint > 1000) { lastPaint = now; InvalidateRect(g_hPcssTweak, NULL, TRUE); }
        }
    } else if (g_hPcssTweak && IsWindowVisible(g_hPcssTweak)) {
        ShowWindow(g_hPcssTweak, SW_HIDE);
    }
}
#endif  // ENABLE_SHADOW_PCSS
