"""Compare two javap -c -p disassemblies modulo benign re-encoding.

Normalizes away everything that legitimately changes when javac rebuilds a
class from equivalent source with a reshuffled constant pool:
  - constant-pool indices (#nnn)      - symbolic comment carries the value
  - bytecode offsets                  - shift when ldc_w narrows to ldc
  - branch / switch targets           - same reason
  - ldc_w vs ldc, ldc2_w vs ldc2     - pool-index width only

Anything left after that is a REAL difference and is printed in full.
Used to prove the CFR round trip of scr104 is instruction-identical before
trusting a recompiled script in the game.
"""
import re, sys, collections, difflib

BRANCH = re.compile(
    r"\b(ifeq|ifne|iflt|ifge|ifgt|ifle|if_icmpeq|if_icmpne|if_icmplt|"
    r"if_icmpge|if_icmpgt|if_icmple|if_acmpeq|if_acmpne|goto_w|goto|jsr|"
    r"ifnull|ifnonnull)\s+\d+")
HEADER = re.compile(
    r"^  (?:(?:public|private|protected|static|final|synchronized) )*"
    r"[\w$.\[\]<>]+ [\w$<>]+\(.*\);$|^  [\w$<>]+\(.*\);$"
    r"|^  static \{\};$")  # <clinit> - javap prints it without parens
INSN = re.compile(r"^\d+: [a-z]")
SWITCH_ENTRY = re.compile(r"^(-?\d+|default): \d+$")


def parse(path):
    methods = collections.OrderedDict()
    cur = None
    for line in open(path, encoding="utf-8", errors="replace"):
        raw = line.rstrip("\n")
        if HEADER.match(raw):
            cur = raw.strip()
            methods[cur] = []
            continue
        if cur is None:
            continue
        s = raw.strip()
        if INSN.match(s):
            t = re.sub(r"^\d+: ", "", s)
            t = re.sub(r"#\d+", "#", t)
            t = re.sub(r"\s+", " ", t)
            t = BRANCH.sub(r"\1 <T>", t)
            t = t.replace("ldc_w", "ldc").replace("ldc2_w", "ldc2")
            methods[cur].append(t)
        elif SWITCH_ENTRY.match(s):
            methods[cur].append(re.sub(r": \d+$", ": <T>", s))
    return methods


def main():
    o = parse(sys.argv[1])
    n = parse(sys.argv[2])
    only_o = [k for k in o if k not in n]
    only_n = [k for k in n if k not in o]
    diff = [k for k in o if k in n and o[k] != n[k]]
    print("methods orig=%d new=%d only-orig=%d only-new=%d REAL-diff=%d"
          % (len(o), len(n), len(only_o), len(only_n), len(diff)))
    for k in only_o:
        print("ONLY-ORIG:", k)
    for k in only_n:
        print("ONLY-NEW :", k)
    for k in diff:
        print("\n=== " + k)
        for l in difflib.unified_diff(o[k], n[k], "orig", "new", lineterm=""):
            print(l)


if __name__ == "__main__":
    main()
