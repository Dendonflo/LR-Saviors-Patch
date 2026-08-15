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

/*
 * hook.c - deliberately a SINGLE TRANSLATION UNIT, assembled from the ordered
 * parts below. Do not compile the parts individually and do not reorder them:
 * the code relies on TU-wide C tentative definitions (the same global declared
 * where each of two distant sections needs it, merged by the compiler) and on
 * hundreds of statics shared across sections. Splitting into separate .c
 * files means de-static-ing and headering all of that - a refactor to do
 * incrementally, not mechanically. The include split keeps the compiler's
 * view byte-identical (verified: the reorganised build differed from the
 * monolithic one only in the PE timestamps) while making the source
 * navigable.
 *
 * build.cmd compiles THIS file; the parts are found relative to it.
 */

#include "hook_parts/01_config_gates.c"
#include "hook_parts/02_interop_provider.c"
#include "hook_parts/03_render_state.c"
#include "hook_parts/04_ssaa.c"
#include "hook_parts/05_script_diag.c"
#include "hook_parts/06_io_alloc.c"
#include "hook_parts/07_timing_watchdog.c"
#include "hook_parts/08_config_persist.c"
#include "hook_parts/09_game_menu.c"
#include "hook_parts/10_overlay.c"
#include "hook_parts/11_monitor.c"
#include "hook_parts/12_locks_staging.c"
#include "hook_parts/13_realdevice_probes.c"
#include "hook_parts/14_aa_shaders.c"
#include "hook_parts/15_msaa.c"
#include "hook_parts/16_output_res_cascade.c"
#include "hook_parts/17_limiter_simdelta.c"
#include "hook_parts/18_fileprobe_shaderid.c"
#include "hook_parts/19_boot_install.c"
#include "hook_parts/20_debug_menu.c"
#include "hook_parts/21_cutscene_shadow.c"
#include "hook_parts/22_d3dx_diag.c"
