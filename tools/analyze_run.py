#!/usr/bin/env python3
"""Analyse version_hook.log stutter data - MARKED RUN SECTION ONLY.

Why this exists (2026-08-15): several rounds of analysis were done over
whole log files, which include boot, title screen, save loading and
teleports. Those phases are far more stutter-dense than gameplay and they
carry completely different causes - class loader, DDS texture decode,
driver shader compilation. Counting them produced a "roster" in which
d3dx and driver work looked like major gameplay stutter sources. Sliced
at the run marker, both are ZERO in gameplay and the heap compactor is
~74% of everything.

The [mark] line (written by toggling shadow distance in the pause menu,
see GameMenuSetSplit in 09_game_menu.c) is what separates them. This tool
always slices at the LAST [mark] and refuses to report on a log without
one, so the mistake cannot be repeated silently.

Usage:
    python tools/analyze_run.py <log> [<log> ...]
    python tools/analyze_run.py --whole <log>    # opt in to full-log stats
"""
import re
import sys
import os
from collections import defaultdict


def classify(rec):
    """Bucket a watchdog record by the subsystem its stack implicates."""
    eip = rec['eip']
    ebp = rec.get('ebp', '')
    if re.search(r'\b00B4[67][0-9A-F]{3}\b', ebp) or eip.startswith(('00B46', '00B47')):
        return 'Heap compactor'
    if re.search(r'\b009[DAF][0-9A-F]{4}\b', ebp) or re.match(r'^009[DAF]', eip):
        return 'Class loader (cause 3)'
    if 'd3dx9' in eip or 'd3dx9' in ebp:
        return 'D3DX texture decode'
    if 'AMDXN32' in eip or 'AMDXN32' in ebp:
        return 'GPU driver (AMDXN32)'
    if 'd3d9.dll' in eip or ('d3d9.dll' in ebp and 'DINPUT8' not in ebp):
        return 'D3D9 runtime'
    if 'KERNELBASE' in ebp and 'DINPUT8' in ebp:
        return 'Engine sync wait (own workers)'
    if eip.startswith(('ntdll', 'win32u')):
        return 'Kernel wait (untraced)'
    if 'USER32' in ebp or 'uxtheme' in ebp:
        return 'Window message pump'
    if 'MSVCR100' in eip:
        return 'CRT (chain lost)'
    return 'Game code (other)'


def parse(path, whole=False):
    """Return (records, frames, over) for the marked run section."""
    lines = open(path, errors='replace').readlines()
    if whole:
        start = 0
    else:
        marks = [i for i, l in enumerate(lines) if l.startswith('[mark]')]
        if not marks:
            return None
        start = marks[-1] + 1

    recs, frames, over, cur = [], 0, 0, None
    for line in lines[start:]:
        if line.startswith('[frametime]'):
            for tok in line.split():
                if tok.startswith('n='):
                    frames += int(tok[2:])
                elif tok.startswith('over='):
                    over += int(tok[5:])
        m = re.match(r'\[stutter\] elapsed_usec=(\d+) EIP=(\S+)', line)
        if m:
            cur = {'us': int(m.group(1)), 'eip': m.group(2)}
            recs.append(cur)
        elif cur is not None:
            if line.startswith('[stutter]   ebp:'):
                cur['ebp'] = line.split(':', 1)[1].strip()
            elif not line.startswith('[stutter]'):
                cur = None
    return recs, frames, over


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    whole = '--whole' in sys.argv
    if not args:
        print(__doc__)
        return 1

    all_recs = []
    print(f'{"run":46s} {"frames":>7s} {"over20ms":>9s} {"caps":>5s} {"30ms+":>6s} {"stall":>8s}')
    for path in args:
        got = parse(path, whole)
        if got is None:
            print(f'{os.path.basename(path):46s}   NO [mark] - run boundary unknown, SKIPPED')
            continue
        recs, frames, over = got
        all_recs += recs
        heavy = sum(1 for r in recs if r['us'] >= 30000)
        stall = sum(r['us'] for r in recs) / 1000.0
        pct = f'{100 * over / frames:.2f}%' if frames else 'n/a'
        print(f'{os.path.basename(path):46s} {frames:7d} {over:4d} {pct:>7s} '
              f'{len(recs):5d} {heavy:6d} {stall:7.0f}ms')

    if not all_recs:
        return 0
    fams = defaultdict(list)
    for r in all_recs:
        fams[classify(r)].append(r['us'])
    total_stall = sum(r['us'] for r in all_recs) / 1000.0
    print(f'\n{len(all_recs)} captures, {total_stall:.0f}ms stall'
          f'{" (WHOLE LOGS - includes loading)" if whole else " (marked gameplay only)"}\n')
    print(f'{"family":34s} {"n":>4s} {"share":>7s} {"median":>7s} {"worst":>7s} {"stall%":>7s}')
    for name, us in sorted(fams.items(), key=lambda kv: -sum(kv[1])):
        s = sorted(us)
        stall = sum(us) / 1000.0
        print(f'{name:34s} {len(us):4d} {100 * len(us) / len(all_recs):6.1f}% '
              f'{s[len(s) // 2] / 1000.0:6.1f}ms {max(us) / 1000.0:6.1f}ms '
              f'{100 * stall / total_stall:6.1f}%')
    return 0


if __name__ == '__main__':
    sys.exit(main())
