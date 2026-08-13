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
#include <windows.h>

// The real dinput8.dll this proxy forwards to. Set by LoadRealDinput8 in
// dllmain.c, which prefers a chain-load target in the game directory before
// falling back to System32 - see the comment there.
extern HMODULE g_hRealDinput8;

void ResolveRealExports(void);
