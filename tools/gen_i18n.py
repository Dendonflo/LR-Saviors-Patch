#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate hook_parts/08b_i18n.c from the readable translation table below.

Why generated rather than hand-written: the C file has to survive MSVC's
source-charset guessing. A UTF-8 file without a BOM is read as the system ANSI
codepage, which turns every Japanese and Korean label into mojibake at compile
time with no diagnostic. Emitting pure-ASCII \\uXXXX escapes removes the guess
entirely - the generated file is 7-bit clean and compiles identically whatever
the codepage.

Edit the TABLE here, re-run, rebuild:

    python tools/gen_i18n.py
"""
import io
import os

# The eight languages LRFF13.exe actually ships. Chinese is TRADITIONAL only -
# the exe's label is the traditional form - so there is no Simplified column to
# fill and nothing to detect if there were.
LANGS = ["en", "fr", "de", "it", "es", "ja", "zh", "ko"]

# The vanilla "Graphics" popup label per language, extracted from LRFF13.exe's
# own localisation block. It stores one "key = value" table per language:
#   EN 0x01d89f18   DE 0x01d8b964   FR 0x01d8d400   IT 0x01d8f250
#   ES 0x01d910fe   KO 0x01d92f12   ZH 0x01d945d6   JA 0x01d95a60
# Detection reads the live menu bar back and matches against these, which is
# the only source that reflects what the game is ACTUALLY rendering - launchers
# such as Nova can override the language behind Steam's back, so Steam's own
# manifest describes what it mounted, not what is on screen.
ANCHORS = {
    "en": "Graphics",
    "fr": "Graphismes",
    "de": "Grafik",
    "it": "Grafica",
    "es": "Gráficos",
    "ja": "グラフィック",
    "zh": "畫面",
    "ko": "그래픽",
}

# id -> per-language text, in the LANGS order above.
# Acronyms (SSAO, HBAO+, MSAA, FXAA, SSAA) and numerals are deliberately NOT
# here: they are written the same way in every localisation of every game.
TABLE = [
    ("S_OFF", [
        "Off", "Désactivé", "Aus", "Disattivato",
        "Desactivado", "オフ", "關閉", "끄기"]),
    ("S_ON", [
        "On", "Activé", "Ein", "Attivato",
        "Activado", "オン", "開啟", "켜기"]),
    ("S_STANDARD", [
        "Standard", "Standard", "Standard", "Standard",
        "Estándar", "標準", "標準", "표준"]),
    ("S_ADVANCED", [
        "Advanced", "Avancé", "Fortgeschritten", "Avanzato",
        "Avanzado", "高度", "高階", "고급"]),
    ("S_HIGH", [
        "High", "Élevé", "Hoch", "Alto",
        "Alto", "高", "高", "높음"]),
    ("S_ULTRA", [
        "Ultra", "Ultra", "Ultra", "Ultra",
        "Ultra", "最高", "極高", "최고"]),
    ("S_UNLIMITED", [
        "Unlimited (unstable, WILL cause bugs)",
        "Illimité (instable, PROVOQUERA des bugs)",
        "Unbegrenzt (instabil, VERURSACHT Fehler)",
        "Illimitato (instabile, CAUSERÀ errori)",
        "Ilimitado (inestable, CAUSARÁ errores)",
        "無制限 (不安定、不具合が発生します)",
        "無限制 (不穩定，會導致故障)",
        "무제한 (불안정, 오류가 발생합니다)"]),
    ("S_SHADOWS", [
        "Shadows", "Ombres", "Schatten", "Ombre",
        "Sombras", "影", "陰影", "그림자"]),
    ("S_SHADOW_DIST", [
        "Shadow Distance", "Distance des ombres", "Schattendistanz",
        "Distanza ombre", "Distancia de sombras",
        "影の描画距離", "陰影距離", "그림자 거리"]),
    ("S_AO", [
        "Ambient Occlusion", "Occlusion ambiante", "Umgebungsverdeckung",
        "Occlusione ambientale", "Oclusión ambiental",
        "アンビエントオクルージョン", "環境光遮蔽", "앰비언트 오클루전"]),
    ("S_TUNING_PANEL", [
        "Tuning Panel", "Panneau de réglages", "Feineinstellungen",
        "Pannello di regolazione", "Panel de ajustes",
        "調整パネル", "調整面板", "조정 패널"]),
    ("S_VSYNC", [
        "VSync", "Synchro verticale", "VSync", "Sincronia verticale",
        "Sincronización vertical", "垂直同期", "垂直同步", "수직 동기화"]),
    ("S_OTHER", [
        "Other", "Autre", "Sonstiges", "Altro",
        "Otros", "その他", "其他", "기타"]),
    ("S_FRAMETIME", [
        "Frametime Overlay", "Graphique de frametime", "Frametime-Overlay",
        "Grafico frametime", "Gráfico de frametime",
        "フレームタイム表示", "影格時間圖表", "프레임타임 표시"]),
    ("S_STATUS_PANEL", [
        "Status Panel", "Panneau d'état", "Statusfenster",
        "Pannello di stato", "Panel de estado",
        "ステータスパネル", "狀態面板", "상태 패널"]),
    # ---- AO tuning window ----
    ("S_AO_TUNING", [
        "AO Tuning", "Réglages AO", "AO-Einstellungen", "Regolazione AO",
        "Ajustes de AO", "AO調整", "AO 調整", "AO 조정"]),
    ("S_STRENGTH", [
        "Strength %", "Intensité %", "Stärke %", "Forza %",
        "Fuerza %", "強度 %", "強度 %", "강도 %"]),
    ("S_INTENSITY", [
        "Intensity", "Contraste", "Intensität", "Intensità",
        "Intensidad", "コントラスト", "對比度", "대비"]),
    ("S_RADIUS", [
        "Radius", "Rayon", "Radius", "Raggio",
        "Radio", "半径", "半徑", "반경"]),
    ("S_BIAS", [
        "Bias", "Biais", "Bias", "Bias",
        "Sesgo", "バイアス", "偏移", "바이어스"]),
    ("S_MAX_RADIUS", [
        "Max Radius %", "Rayon max %", "Max. Radius %", "Raggio max %",
        "Radio máx. %", "最大半径 %", "最大半徑 %", "최대 반경 %"]),
    ("S_BLUR_SHARP", [
        "Blur Sharp", "Netteté du flou", "Unschärfe-Kantenschutz",
        "Nitidezza sfocatura", "Nitidez del desenfoque",
        "ぼかしの鋭さ", "模糊銳度", "블러 선명도"]),
    ("S_BLUR_PASSES", [
        "Blur Passes", "Passes de flou", "Unschärfe-Durchgänge",
        "Passaggi sfocatura", "Pasadas de desenfoque",
        "ぼかし回数", "模糊次數", "블러 횟수"]),
    ("S_BLUR_SPREAD", [
        "Blur Spread", "Étalement du flou", "Unschärfe-Streuung",
        "Diffusione sfocatura", "Extensión del desenfoque",
        "ぼかしの広がり", "模糊範圍", "블러 범위"]),
    ("S_BLUR_NV", [
        "NVIDIA blur", "Flou NVIDIA", "NVIDIA-Weichzeichner",
        "Sfocatura NVIDIA", "Desenfoque NVIDIA",
        "NVIDIA ブラー", "NVIDIA 模糊", "NVIDIA 블러"]),
    ("S_SHOW_RAW", [
        "Show raw AO", "Afficher l'AO brute", "Rohes AO anzeigen",
        "Mostra AO grezza", "Mostrar AO en bruto",
        "生のAOを表示", "顯示原始 AO", "원본 AO 표시"]),
    ("S_SHOW_RAW_OFF", [
        "Show raw AO  -  turn AO on first",
        "Afficher l'AO brute  -  activez l'AO d'abord",
        "Rohes AO anzeigen  -  AO zuerst einschalten",
        "Mostra AO grezza  -  attiva prima l'AO",
        "Mostrar AO en bruto  -  activa antes la AO",
        "生のAOを表示  -  先にAOをオンに",
        "顯示原始 AO  -  請先開啟 AO",
        "원본 AO 표시  -  먼저 AO를 켜세요"]),
    ("S_AO_RES", [
        "AO Resolution", "Résolution de l'AO", "AO-Auflösung",
        "Risoluzione AO", "Resolución de AO",
        "AOの解像度", "AO 解析度", "AO 해상도"]),
    ("S_RES_NATIVE", [
        "Native (expensive at high res)",
        "Native (coûteux en haute résolution)",
        "Nativ (teuer bei hoher Auflösung)",
        "Nativa (onerosa ad alta risoluzione)",
        "Nativa (costosa a alta resolución)",
        "ネイティブ (高解像度では重い)",
        "原生 (高解析度下開銷大)",
        "원본 해상도 (고해상도에서 무거움)"]),
    ("S_RES_HALF", [
        "Half", "Moitié", "Halb", "Metà",
        "Mitad", "1/2", "二分之一", "1/2"]),
    ("S_RES_QUARTER", [
        "Quarter", "Quart", "Viertel", "Quarto",
        "Cuarto", "1/4", "四分之一", "1/4"]),
    # ---- reset actions ----
    ("S_RESET_ALL", [
        "Reset all settings", "Réinitialiser tous les réglages",
        "Alle Einstellungen zurücksetzen", "Reimposta tutte le impostazioni",
        "Restablecer todos los ajustes", "すべての設定をリセット",
        "重設所有設定", "모든 설정 초기화"]),
    ("S_RESET_AO", [
        "Reset AO settings", "Réinitialiser les réglages AO",
        "AO-Einstellungen zurücksetzen", "Reimposta le impostazioni AO",
        "Restablecer los ajustes de AO", "AO設定をリセット",
        "重設 AO 設定", "AO 설정 초기화"]),
    ("S_RESET_ASK_ALL", [
        "Reset every setting to its default value?",
        "Réinitialiser tous les réglages à leur valeur par défaut ?",
        "Alle Einstellungen auf ihre Standardwerte zurücksetzen?",
        "Reimpostare tutte le impostazioni ai valori predefiniti?",
        "¿Restablecer todos los ajustes a sus valores predeterminados?",
        "すべての設定を初期値に戻しますか?",
        "要將所有設定重設為預設值嗎?",
        "모든 설정을 기본값으로 되돌릴까요?"]),
    ("S_RESET_ASK_AO", [
        "Reset the AO tuning values to their defaults?",
        "Réinitialiser les réglages AO à leurs valeurs par défaut ?",
        "Die AO-Feineinstellungen auf ihre Standardwerte zurücksetzen?",
        "Reimpostare i valori di regolazione AO ai predefiniti?",
        "¿Restablecer los valores de ajuste de AO a sus predeterminados?",
        "AOの調整値を初期値に戻しますか?",
        "要將 AO 調整值重設為預設值嗎?",
        "AO 조정 값을 기본값으로 되돌릴까요?"]),
]


def esc(s):
    out = []
    for ch in s:
        o = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif 0x20 <= o < 0x7F:
            out.append(ch)
        else:
            out.append("\\u%04X" % o)
    return "".join(out)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    dst = os.path.join(root, "mods", "version_hook", "hook_parts", "08b_i18n.c")
    L = []
    w = L.append
    w("// GENERATED by tools/gen_i18n.py - do not edit by hand.")
    w("// Edit the table in that script and re-run it.")
    w("//")
    w("// Pure ASCII on purpose: every non-Latin character is a \\uXXXX escape, so")
    w("// MSVC never has to guess the source charset. A UTF-8 file without a BOM")
    w("// would be read as the system ANSI codepage and silently mangle every")
    w("// Japanese and Korean label with no diagnostic at all.")
    w("")
    w("typedef enum {")
    for i, code in enumerate(LANGS):
        w("    LANG_%s = %d," % (code.upper(), i))
    w("    LANG_COUNT")
    w("} ModLang;")
    w("")
    w("typedef enum {")
    for name, _ in TABLE:
        w("    %s," % name)
    w("    S_COUNT")
    w("} ModStr;")
    w("")
    w("// Resolved by LangDetectFromMenu() once the vanilla menu exists - it reads")
    w("// the game's own popup labels back. Index into the table below.")
    w("static volatile LONG g_langIdx = LANG_EN;")
    w("")
    w("static const wchar_t *const g_i18n[S_COUNT][LANG_COUNT] = {")
    for name, vals in TABLE:
        assert len(vals) == len(LANGS), "%s has %d entries, need %d" % (
            name, len(vals), len(LANGS))
        w("    /* %s */ {" % name)
        for code, v in zip(LANGS, vals):
            w('        /* %-2s */ L"%s",' % (code, esc(v)))
        w("    },")
    w("};")
    w("")
    w("// Every label goes through this. g_langIdx is clamped on assignment, so")
    w("// no bounds check here.")
    w("#define TR(id) (g_i18n[(id)][g_langIdx])")
    w("")
    w('// The vanilla "Graphics" popup label per language, lifted from the exe\'s')
    w("// own localisation table. Detection reads the live menu bar back and")
    w("// matches against these - see 08c_lang_detect.c.")
    w("static const wchar_t *const g_langAnchor[LANG_COUNT] = {")
    for code in LANGS:
        w('    /* %-2s */ L"%s",' % (code, esc(ANCHORS[code])))
    w("};")
    w("")
    io.open(dst, "w", encoding="ascii", newline="\r\n").write("\n".join(L))
    print("wrote %s (%d strings x %d languages)" % (dst, len(TABLE), len(LANGS)))


if __name__ == "__main__":
    main()
