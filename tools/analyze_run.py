#!/usr/bin/env python3
"""Analyse SaviorsPatch.log stutter data, per MARKED SEGMENT.

Why this exists (2026-08-15): several rounds of analysis were done over
whole log files, which include boot, title screen, save loading and
teleports. Those phases are far more stutter-dense than gameplay and they
carry entirely different causes - class loader, DDS texture decode,
driver shader compilation. Counting them produced a "roster" in which
d3dx and driver work looked like major gameplay stutter sources. Sliced
at the run marker, all three are ZERO in gameplay and the heap compactor
dominated.

Marks come from Optimization > Mark Log (run start) in the pause menu
(MenuH_LogMark in 09_game_menu.c), which writes:

    [mark] #1  (12:07:54  up=12609.1s  frame=1939)

Everything before the first mark is treated as preamble and reported
separately, never folded into gameplay totals. Each further mark starts a
new segment, so a multi-zone run can be marked at every zone change and
compared zone by zone.

Usage:
    python tools/analyze_run.py <log> [<log> ...]
    python tools/analyze_run.py --whole <log>     # opt in to full-log stats
    python tools/analyze_run.py --quiet <log>     # segment table only
"""
import re
import sys
import os
from collections import defaultdict

# Permissive on purpose: matches both the current "[mark] #1  (12:07:54 ..."
# and the retired "[mark] shadow distance -> 300%  (08:45:29 ..." form, so the
# pre-2026-08-15 archives stay comparable.
MARK_RE = re.compile(r'^\[mark\]\s*(?:#(\d+))?[^(]*\((\d\d:\d\d:\d\d)')
STUTTER_RE = re.compile(r'\[stutter\] elapsed_usec=(\d+) EIP=(\S+)')


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


def segment(lines):
    """Yield (label, body_lines). First segment is the pre-mark preamble."""
    bounds = []
    for i, l in enumerate(lines):
        m = MARK_RE.match(l)
        if m:
            num = m.group(1) or str(len(bounds) + 1)
            bounds.append((i, f'#{num} @{m.group(2)}'))
    if not bounds:
        return None
    out = [('preamble (boot/load)', lines[:bounds[0][0]])]
    for k, (idx, label) in enumerate(bounds):
        end = bounds[k + 1][0] if k + 1 < len(bounds) else len(lines)
        out.append((label, lines[idx + 1:end]))
    return out


def scan(body):
    """Return (records, frames, over) for a block of log lines."""
    recs, frames, over, cur = [], 0, 0, None
    for line in body:
        if line.startswith('[frametime]'):
            for tok in line.split():
                if tok.startswith('n='):
                    frames += int(tok[2:])
                elif tok.startswith('over='):
                    over += int(tok[5:])
        m = STUTTER_RE.match(line)
        if m:
            cur = {'us': int(m.group(1)), 'eip': m.group(2)}
            recs.append(cur)
        elif cur is not None:
            if line.startswith('[stutter]   ebp:'):
                cur['ebp'] = line.split(':', 1)[1].strip()
            elif not line.startswith('[stutter]'):
                cur = None
    return recs, frames, over


def census(recs, title, indent='  '):
    if not recs:
        print(f'{indent}(no captures)')
        return
    fams = defaultdict(list)
    for r in recs:
        fams[classify(r)].append(r['us'])
    total_stall = sum(r['us'] for r in recs) / 1000.0
    print(f'{indent}{title}')
    for name, us in sorted(fams.items(), key=lambda kv: -sum(kv[1])):
        s = sorted(us)
        stall = sum(us) / 1000.0
        print(f'{indent}  {name:32s} {len(us):4d} {100 * len(us) / len(recs):5.1f}% '
              f'med {s[len(s) // 2] / 1000.0:5.1f}ms  worst {max(us) / 1000.0:5.1f}ms '
              f'  {100 * stall / total_stall:5.1f}% stall')


def row(label, frames, over, recs):
    heavy = sum(1 for r in recs if r['us'] >= 30000)
    stall = sum(r['us'] for r in recs) / 1000.0
    pct = f'{100 * over / frames:.2f}%' if frames else '  n/a'
    print(f'  {label:24s} {frames:6d} {over:5d} {pct:>7s} {len(recs):5d} '
          f'{heavy:5d} {stall:7.0f}ms')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    whole = '--whole' in sys.argv
    quiet = '--quiet' in sys.argv
    if not args:
        print(__doc__)
        return 1

    grand = []
    for path in args:
        lines = open(path, errors='replace').readlines()
        print(f'\n=== {os.path.basename(path)} ===')

        if whole:
            recs, frames, over = scan(lines)
            print(f'  {"segment":24s} {"frames":>6s} {"over":>5s} {"rate":>7s} '
                  f'{"caps":>5s} {"30ms+":>5s} {"stall":>9s}')
            row('WHOLE LOG (inc. load)', frames, over, recs)
            census(recs, 'families (WHOLE LOG - includes loading):')
            grand += recs
            continue

        segs = segment(lines)
        if segs is None:
            print('  NO [mark] - run boundary unknown, SKIPPED.')
            print('  Mark runs via Optimization > Mark Log, or pass --whole '
                  'to accept loading-contaminated stats.')
            continue

        print(f'  {"segment":24s} {"frames":>6s} {"over":>5s} {"rate":>7s} '
              f'{"caps":>5s} {"30ms+":>5s} {"stall":>9s}')
        gameplay, gp_frames, gp_over = [], 0, 0
        for label, body in segs:
            recs, frames, over = scan(body)
            row(label, frames, over, recs)
            if not label.startswith('preamble'):
                gameplay += recs
                gp_frames += frames
                gp_over += over
        if len(segs) > 2:
            row('ALL MARKED', gp_frames, gp_over, gameplay)

        if not quiet:
            print()
            for label, body in segs:
                if label.startswith('preamble'):
                    continue
                recs, _, _ = scan(body)
                if recs:
                    census(recs, f'segment {label}:')
                    print()
            if len(segs) > 2:
                census(gameplay, 'ALL MARKED SEGMENTS COMBINED:')
        grand += gameplay

    if len(args) > 1 and grand:
        print(f'\n=== combined across {len(args)} logs ===')
        census(grand, f'{len(grand)} captures:')
    return 0


if __name__ == '__main__':
    sys.exit(main())
