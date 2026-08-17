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
#define MOD_NAME        "Savior's Patch"
#define MOD_TAGLINE     "Performance & graphics"
// Bump for each release. Logged in the boot banner, which is the first line of
// every log and the one that makes a user's bug report actionable.
#define MOD_VERSION     "1.0"
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
