"""Release gate: the built DLL must not carry scanner-bait vocabulary in its
strings. See the STRING VOCABULARY note in mods/version_hook/hook.h.

    python tools/check_strings.py [path-to-dll]
"""
import re, sys, os
p = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "mods", "version_hook", "dinput8_new.dll")
d = open(p, "rb").read()
kw = re.compile(r"\b(hook|hooks|hooked|hooking|vtable|vtables|IAT|patch|patches|patched|patching|inject|injection|injected|trampoline|detour)\b", re.I)
hits = sorted({m.group().decode() for m in re.finditer(rb"[\x20-\x7e]{5,}", d)
               if kw.search(m.group().decode()) and b"Savior's Patch" not in m.group()})
for h in hits:
    print("  BAIT:", h[:110])
print("STRINGS OK" if not hits else f"{len(hits)} bait string(s) - fix before release")
sys.exit(1 if hits else 0)
