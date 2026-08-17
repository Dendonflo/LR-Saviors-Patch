// Language resolution for the mod's own menu labels (08b_i18n.c holds the
// table). Hand-written; 08b is generated and gets overwritten.
//
// The problem this solves: the game's own menu is localised, ours was not, so
// a French player saw "Ombres" directly above "Ambient Occlusion". There is no
// route to the engine's own localisation for OUR entries - its label table is
// keyed by EXE STRING POINTERS, so a name it does not already know misses the
// lookup, the builder inserts its "*" fallback, and we overwrite that with a
// literal (see GameMenuFixLabel). Hence our own table.
//
// HOW THE LANGUAGE IS FOUND: by reading the game's OWN menu back.
//
// After the vanilla tree is built, every top-level popup on the menu bar is
// read with GetMenuStringW and compared against the localised "Graphics"
// label for each language (g_langAnchor, lifted from the exe's own
// localisation block). Whatever the game is rendering is, by definition, the
// language the game is in.
//
// This is the ONLY source that is right in every case, and the alternatives
// were tried and rejected:
//
//   Steam's app manifest (appmanifest_345350.acf, UserConfig/MountedConfig
//   language) looks authoritative and is not. Third-party launchers - Nova in
//   particular - override the game's language independently of Steam, so the
//   manifest describes what Steam mounted rather than what is on screen. Set
//   German in Nova on a French Steam install and the manifest still says
//   french while the menu says Grafik: the mod would then print French labels
//   into a German menu, which is the exact bug this file exists to remove.
//
//   The OS UI language is wrong whenever someone plays in a language other
//   than their desktop, which is common and needs no launcher at all.
//
// Detection therefore has to happen after the menu exists rather than at
// startup, which is why this is called from the menu build path and not from
// LoadConfig. Until it runs, the ini override (or English) applies - and no
// label of ours is inserted before it, because insertion happens immediately
// after the same vanilla build.

// Compare a menu label against an anchor. Exact match: the anchors come from
// the same exe that renders the menu, so there is nothing to normalise.
static int LangMatchAnchor(const wchar_t *label)
{
    int i;
    for (i = 0; i < LANG_COUNT; i++)
        if (wcscmp(label, g_langAnchor[i]) == 0)
            return i;
    return -1;
}

// Walk the menu bar's top-level popups looking for the Graphics one.
static int LangFromMenuBar(HMENU bar)
{
    int n, i;
    if (!bar || !IsMenu(bar)) return -1;
    n = GetMenuItemCount(bar);
    for (i = 0; i < n; i++) {
        wchar_t buf[128];
        int got = GetMenuStringW(bar, (UINT)i, buf, 128, MF_BYPOSITION);
        if (got > 0) {
            int lang = LangMatchAnchor(buf);
            if (lang >= 0) return lang;
        }
    }
    return -1;
}

static int LangFromOsUi(void)
{
    LANGID id = LANGIDFROMLCID(GetUserDefaultUILanguage());
    switch (PRIMARYLANGID(id)) {
    case LANG_FRENCH:   return LANG_FR;
    case LANG_GERMAN:   return LANG_DE;
    case LANG_ITALIAN:  return LANG_IT;
    case LANG_SPANISH:  return LANG_ES;
    case LANG_JAPANESE: return LANG_JA;
    case LANG_KOREAN:   return LANG_KO;
    case LANG_CHINESE:  return LANG_ZH;   // the game ships Traditional only
    default:            return LANG_EN;
    }
}

// Called from the menu build path, every rebuild. Cheap (a handful of
// GetMenuStringW calls) and re-running it costs nothing, but it also means a
// language change mid-session is picked up rather than cached wrong.
static void LangDetectFromMenu(HMENU bar)
{
    static LONG announced = -1;
    const char *how;
    int lang;

    if (g_langCfg > 0 && g_langCfg <= LANG_COUNT) {
        lang = (int)g_langCfg - 1;          // ini is 1-based; 0 means auto
        how = "ini Language=";
    } else {
        lang = LangFromMenuBar(bar);
        how = "game menu label";
        if (lang < 0) {
            lang = LangFromOsUi();
            how = "OS UI language (menu label not matched)";
        }
    }
    if (lang < 0 || lang >= LANG_COUNT) lang = LANG_EN;
    InterlockedExchange(&g_langIdx, lang);

    if (announced != lang) {
        static const char *const names[LANG_COUNT] = {
            "english", "french", "german", "italian",
            "spanish", "japanese", "chinese-trad", "korean"
        };
        char l[128];
        announced = lang;
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
