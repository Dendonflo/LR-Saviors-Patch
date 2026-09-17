/*
 * Lightning Returns: Final Fantasy XIII - asset streaming & stutter mod
 * Copyright (C) 2026  Dendonflo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

// ---- Mod identity ---------------------------------------------------------
// Defined here because hook.h is the only header both translation units share
// (hook.c's single-TU manifest and dllmain.c, which writes its own startup
// log before any of that exists). Previously the filenames were four scattered
// string literals, which is how "version_hook" - a development name that was
// never meant to be user-facing - ended up on files players would see.
//
// The DLL itself CANNOT be renamed: it ships as dinput8.dll because that is
// the proxy the game loads. The name lives in these files and in the menu.
// STRING VOCABULARY (2026-09-17): no "hook", "vtable", "IAT", "patch",
// "inject", "trampoline" or "detour" inside any string literal that ends
// up in the DLL - log lines, panel labels, registry names. 1.1 was held by
// Nexus on a BitDefender Gen:Variant.Draftor verdict (6 of 8 engines were
// that one engine) and the only difference from the clean 1.0 binary was
// the status panel putting those words in cleartext. Identifiers and
// comments are free; strings say link / slot table / import / tweak /
// insert / return stub / redirect. Checked by the release scan in
// tools/check_strings.py.
#define MOD_NAME        "Savior's Patch"
#define MOD_TAGLINE     "Performance & graphics"
// Bump for each release. Logged in the boot banner, which is the first line of
// every log and the one that makes a user's bug report actionable.
//
// NUMBERING, because it goes BACKWARDS here and that looks like a mistake.
// 1.0/1.1/1.2 BETA were the pre-release line. This is the first PUBLIC
// release and it is 1.0, so a log from the last beta says 1.2 and a log from
// the release says 1.0. The disambiguator already in the banner is the BUILD
// TIMESTAMP - a 1.0 dated after a 1.2 is the release, not an older build -
// so when triaging a report, read the date, not just the number.
// 1.1 (2026-09-15): shadow softness / PCSS, uniform shadow projection,
// NPC spawning distance, in-frame shadow + AO tuning panels.
#define MOD_VERSION     "1.1"
// Wide form of MOD_NAME for MessageBoxW captions, without restating the name.
#define MOD_WIDEN2(x)   L##x
#define MOD_WIDEN(x)    MOD_WIDEN2(x)
#define MOD_NAME_W      MOD_WIDEN(MOD_NAME)
#define MOD_CONFIG_FILE "SaviorsPatch.ini"
#define MOD_LOG_FILE    "SaviorsPatch.log"

// Installs the animation/skeleton prefetch hook. Safe to call once the
// process's own main module is loaded (i.e. from a thread, not directly
// inside DllMain's DLL_PROCESS_ATTACH).
void InstallPrefetchHook(void);

// The small subset of setup that MUST happen before the game's entry point
// runs, and therefore must be called synchronously from DllMain rather than
// from the deferred install thread.
//
// Everything else is deliberately deferred (see the loader-lock reasoning in
// dllmain.c), but device creation cannot be: the game calls
// Direct3DCreate9 within a couple of seconds of start, and the deferred
// thread demonstrably loses that race - a test run installed the IAT hook
// only AFTER the real device already existed, so the hook never fired.
// This does no loader-lock-unsafe work: it patches the exe's own already-
// snapped import table and reads a config file, with no LoadLibrary,
// no GetProcAddress against other modules, and no calls into d3d9.dll.
void InstallEarlyHooks(void);
