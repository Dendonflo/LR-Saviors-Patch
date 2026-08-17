// Language resolution for the mod's own menu labels (08b_i18n.c holds the
// table). Hand-written; 08b is generated and gets overwritten.
//
// The problem this solves: the game's own menu is localised, ours was not, so
// a French player saw "Ombres" next to "Ambient Occlusion". The engine builds
// vanilla labels through a table keyed by EXE STRING POINTERS, so our names
// miss it, the builder inserts its "*" fallback, and we overwrite that with a
// literal (see GameMenuFixLabel). There is no route to the engine's
// localisation for a name it does not already know - hence our own table.
//
// WHERE THE LANGUAGE COMES FROM, in order:
//
//   1. ini Language=N, an explicit override. Always wins.
//   2. Steam's app manifest. This is the good one: the game ships as a SINGLE
//      22GB depot containing every language, and Steam records the selected
//      one as a plain token in appmanifest_345350.acf. MountedConfig is what
//      Steam actually mounted for the session, UserConfig what the user
//      picked; prefer the former. Verified on the dev machine, which reads
//      "french" in both.
//   3. The OS UI language. Wrong whenever the player runs the game in a
//      language other than their desktop, which is common, so it is a
//      fallback rather than a source.
//   4. English.
//
// A label fingerprint (read a vanilla menu item back with GetMenuStringW and
// match it) was the original plan and is deliberately NOT used: it needs the
// menu to exist before it can answer, it needs a per-language table of the
// game's own words to compare against, and the manifest already states the
// answer outright before a single frame is drawn.

#define LANG_ACF_NAME "appmanifest_345350.acf"   // LR:FFXIII on Steam

static int LangFromSteamToken(const char *tok, size_t n)
{
    size_t i;
    for (i = 0; i < sizeof(g_steamLangs) / sizeof(g_steamLangs[0]); i++) {
        const char *t = g_steamLangs[i].token;
        if (strlen(t) == n && _strnicmp(t, tok, n) == 0)
            return g_steamLangs[i].lang;
    }
    return -1;
}

// Pull the value of the LAST `"language" "<token>"` pair inside the block
// starting at `from`. The ACF is a tiny, flat, quoted-token format - no need
// for a parser, and a malformed file simply yields no match.
static int LangScanAcf(const char *buf, const char *from)
{
    const char *p = from ? from : buf;
    const char *hit = NULL;
    while ((p = strstr(p, "\"language\"")) != NULL) {
        hit = p;
        p += 10;
    }
    if (!hit) return -1;
    p = hit + 10;
    while (*p && *p != '"') p++;          // opening quote of the value
    if (!*p) return -1;
    {
        const char *s = ++p;
        while (*p && *p != '"') p++;
        return LangFromSteamToken(s, (size_t)(p - s));
    }
}

static int LangFromSteamManifest(void)
{
    char path[MAX_PATH], *slash;
    HANDLE h;
    DWORD got = 0, size;
    char *buf;
    int lang = -1;

    if (!GetModuleFileNameA(NULL, path, MAX_PATH)) return -1;
    // <library>\steamapps\common\<installdir>\LRFF13.exe
    //  -> strip exe, strip <installdir>, strip "common", append the manifest.
    {
        int up;
        for (up = 0; up < 3; up++) {
            slash = strrchr(path, '\\');
            if (!slash) return -1;
            *slash = 0;
        }
    }
    if (strlen(path) + 1 + strlen(LANG_ACF_NAME) + 1 >= MAX_PATH) return -1;
    strcat(path, "\\" LANG_ACF_NAME);

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > (1u << 20)) { CloseHandle(h); return -1; }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) { CloseHandle(h); return -1; }
    if (ReadFile(h, buf, size, &got, NULL) && got) {
        buf[got] = 0;
        // MountedConfig is what Steam actually mounted this session; fall back
        // to a whole-file scan, which finds UserConfig's copy.
        lang = LangScanAcf(buf, strstr(buf, "MountedConfig"));
        if (lang < 0) lang = LangScanAcf(buf, NULL);
    }
    free(buf);
    CloseHandle(h);
    return lang;
}

static int LangFromOsUi(void)
{
    switch (PRIMARYLANGID(LANGIDFROMLCID(GetUserDefaultUILanguage()))) {
    case LANG_FRENCH:   return LANG_FR;
    case LANG_GERMAN:   return LANG_DE;
    case LANG_ITALIAN:  return LANG_IT;
    case LANG_SPANISH:  return LANG_ES;
    case LANG_JAPANESE: return LANG_JA;
    case LANG_KOREAN:   return LANG_KO;
    case LANG_CHINESE:
        // Traditional for TW/HK/MO, Simplified otherwise.
        switch (SUBLANGID(LANGIDFROMLCID(GetUserDefaultUILanguage()))) {
        case SUBLANG_CHINESE_TRADITIONAL:
        case SUBLANG_CHINESE_HONGKONG:
        case SUBLANG_CHINESE_MACAU:
            return LANG_ZHT;
        default:
            return LANG_ZHS;
        }
    default: return LANG_EN;
    }
}

static void LangDetect(void)
{
    const char *how = "default";
    int lang = -1;

    if (g_langCfg > 0 && g_langCfg <= LANG_COUNT) {
        lang = (int)g_langCfg - 1;          // ini is 1-based; 0 means auto
        how = "ini Language=";
    }
    if (lang < 0) {
        lang = LangFromSteamManifest();
        if (lang >= 0) how = "Steam app manifest";
    }
    if (lang < 0) {
        lang = LangFromOsUi();
        how = "OS UI language";
    }
    if (lang < 0 || lang >= LANG_COUNT) lang = LANG_EN;
    InterlockedExchange(&g_langIdx, lang);
    {
        static const char *const names[LANG_COUNT] = {
            "english", "french", "german", "italian", "spanish",
            "japanese", "schinese", "tchinese", "koreana"
        };
        char l[128];
        sprintf(l, "[i18n] menu language: %s (via %s)", names[lang], how);
        LogLine(l);
    }
}

// Compose "<translated> (1024)" and friends. InsertMenuItemW/SetMenuItemInfoW
// COPY the string into the menu, so one shared buffer is safe - the value is
// consumed before the next call needs it.
static const wchar_t *TrSuffix(int id, const wchar_t *suffix)
{
    static wchar_t buf[160];
    _snwprintf(buf, 160, L"%s %s", g_i18n[id][g_langIdx], suffix);
    buf[159] = 0;
    return buf;
}
