#!/usr/bin/env python3
"""Compile the injected HLSL strings offline, before they ever reach the game.

The AO shaders live as concatenated C string literals in 25_ssao.c and are
compiled at RUNTIME by D3DXCompileShader. A syntax error therefore produces
no build failure at all - it produces a NULL pixel shader, a single line in
SaviorsPatch.log, and an effect that silently does nothing. That is exactly
how commit 6a8be5b shipped an SSAO variant whose `for` loop header had been
deleted: the C compiled, the deploy succeeded, and only the game knew.

This extracts each shader string and runs it through fxc for ps_3_0, with the
same macro sets AoEnsureShaders uses, so the failure surfaces here instead.

    python tools/check_shaders.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "mods", "version_hook", "hook_parts", "25_ssao.c")
OUT = os.path.join(ROOT, "build", "shadercheck")

# Mirrors g_aoQTaps / g_aoQDirs / g_aoQSteps / g_aoQTurns in 25_ssao.c.
QUALITY = [
    ("8", "3", "4", "3"),
    ("12", "4", "4", "5"),
    ("20", "6", "4", "9"),
]

STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def find_fxc():
    roots = [
        r"C:\Program Files (x86)\Windows Kits\10\bin",
        r"C:\Program Files\Windows Kits\10\bin",
    ]
    found = []
    for r in roots:
        for dirpath, _dirnames, filenames in os.walk(r) if os.path.isdir(r) else []:
            if "fxc.exe" in filenames and os.sep + "x64" + os.sep in dirpath + os.sep:
                found.append(os.path.join(dirpath, "fxc.exe"))
    if not found:
        return None
    return sorted(found)[-1]


def extract(csrc, name):
    """Pull one `static const char *NAME = "..." "...";` literal out of C source."""
    lines = csrc.splitlines()
    start = None
    for i, ln in enumerate(lines):
        if re.match(r"\s*static\s+const\s+char\s*\*\s*" + re.escape(name) + r"\s*=", ln):
            start = i
            break
    if start is None:
        raise SystemExit("could not find shader string %s in %s" % (name, SRC))

    parts = []
    for ln in lines[start:]:
        code = ln
        stripped = code.strip()
        if stripped.startswith("//"):
            continue  # a whole-line C comment; may legitimately contain quotes
        for m in STR_RE.finditer(code):
            parts.append(m.group(1))
        # The literal ends at the first `;` that is outside a string.
        if re.search(r'"\s*;\s*(//.*)?$', code):
            break
    text = "".join(parts)
    # Unescape the C escapes that appear in these strings.
    return text.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"')


def compile_one(fxc, hlsl, defines, label):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, re.sub(r"[^A-Za-z0-9]+", "_", label) + ".hlsl")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(hlsl)
    cmd = [fxc, "/nologo", "/T", "ps_3_0", "/E", "main", "/Fo", path + ".fxo"]
    for k, v in defines:
        cmd += ["/D", "%s=%s" % (k, v)]
    cmd.append(path)
    p = subprocess.run(cmd, capture_output=True, text=True)
    ok = p.returncode == 0
    print(("  PASS  " if ok else "  FAIL  ") + label)
    if not ok:
        out = (p.stdout + p.stderr).strip()
        for ln in out.splitlines():
            if ln.strip():
                print("        " + ln)
    return ok


def nv_blur_enabled():
    """Read the ENABLE_NV_BLUR gate out of 01_config_gates.c.

    Parsed rather than hardcoded so flipping the gate back on cannot leave the
    checker silently skipping a shader that is once again being compiled at
    runtime - which is the exact failure this whole tool exists to catch.
    """
    gates = os.path.join(os.path.dirname(SRC), "01_config_gates.c")
    try:
        with open(gates, encoding="utf-8", errors="replace") as fh:
            m = re.search(r"^#define\s+ENABLE_NV_BLUR\s+(\d+)", fh.read(), re.M)
    except OSError:
        return False
    return bool(m and int(m.group(1)))


def main():
    fxc = find_fxc()
    if not fxc:
        print("fxc.exe not found - install the Windows 10 SDK")
        return 2
    print("fxc: %s" % fxc)

    with open(SRC, encoding="utf-8", errors="replace") as fh:
        csrc = fh.read()

    ok = True
    estimator = extract(csrc, "g_ssaoHlsl")
    for est, estname in ((0, "SSAO"), (1, "HBAO")):
        for q, (taps, dirs, steps, turns) in enumerate(QUALITY):
            defs = [
                ("ESTIMATOR", str(est)),
                ("AO_TAPS", taps),
                ("AO_DIRS", dirs),
                ("AO_STEPS", steps),
                ("AO_TURNS", turns),
            ]
            ok &= compile_one(fxc, estimator, defs, "%s q%d" % (estname, q))

    # g_aoBlurNvHlsl is checked only while ENABLE_NV_BLUR is on. It is retired
    # (worse than ours on SSAO, indistinguishable on HBAO+) but still compiled
    # here when the gate is flipped back, so reviving it cannot ship broken.
    blurs = [("g_aoBlurHlsl", "blur")]
    if nv_blur_enabled():
        blurs.append(("g_aoBlurNvHlsl", "blur-nvidia"))
    blurs.append(("g_aoCombineHlsl", "combine"))
    for name, label in blurs:
        ok &= compile_one(fxc, extract(csrc, name), [], label)

    # PCSS shadow projection (27_shadow_pcss.c), same macro set PcssEnsure uses.
    pcss_src = os.path.join(os.path.dirname(SRC), "27_shadow_pcss.c")
    if os.path.exists(pcss_src):
        with open(pcss_src, encoding="utf-8", errors="replace") as fh:
            pcss_c = fh.read()
        ok &= compile_one(fxc, extract(pcss_c, "g_pcssHlsl"),
                          [("PCSS_TAPS", "32"), ("PCSS_SEARCH", "16")], "PCSS")

    print("ALL SHADERS OK" if ok else "SHADER CHECK FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
