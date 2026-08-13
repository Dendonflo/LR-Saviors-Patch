"""
Extract and decompile every LR:FFXIII .clb script in one pass.

Pipeline (all pure Python except the CFR decompile step):

    weiss_data/**/*.clb
      -> clb_crypto.decrypt()      (self-keyed block cipher, no external secret)
      -> clb_to_class.convert()    (CLB address layout -> real JVM classfile)
      -> classes/<internal/name>.class
      -> CFR, ONE JAR PER GROUP    -> java/<group>/...

Why the grouping matters: zone scripts in different zones compile to the SAME
bare class names (scr000, scr169, ...). Decompiling them together makes CFR
pick one arbitrarily and drop the rest, and decompiling them one file at a
time leaves every cross-reference unresolved - which is exactly the "Could
not load the following classes: cmn.common, fld.com, scr169, ..." header seen
on the earlier one-off GUI extraction. So: one group for sys/script (package-
qualified, collision-free) and one group per zone.

Adapted from the FF13-2 pipeline in FF13HDMod/tools (same CLB format); the
differences are the data root (weiss_data vs alba_data) and the layout of
zone directories.
"""
import glob
import os
import shutil
import subprocess
import sys
import zipfile
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from clb_crypto import decrypt
from clb_to_class import convert
from jvm_class_name import get_class_name

GAME_ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\LIGHTNING RETURNS FINAL FANTASY XIII"
DATA_DIR = os.path.join(GAME_ROOT, "weiss_data")
CFR_JAR = r"C:\Users\dendo\Downloads\Nova Chrysalia v2.0.9\Libraries\cfr_0_110.jar"

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "script_research"))
CLASSES_DIR = os.path.join(OUT_ROOT, "classes")
JARS_DIR = os.path.join(OUT_ROOT, "jars")
JAVA_DIR = os.path.join(OUT_ROOT, "java")


def group_of(clb_path: str) -> str:
    """sys/script/** -> 'sys'; zone/z0100/** -> 'z0100'; anything else by dir."""
    rel = os.path.relpath(clb_path, DATA_DIR).replace("\\", "/")
    parts = rel.split("/")
    if parts[0] == "sys":
        return "sys"
    if parts[0] == "zone" and len(parts) > 1:
        return parts[1]
    return parts[0]


def stage_convert():
    """Decrypt + translate every .clb into classes/<group>/<internal/name>.class."""
    clb_files = sorted(glob.glob(os.path.join(DATA_DIR, "**", "*.clb"), recursive=True))
    print(f"[1/3] found {len(clb_files)} .clb files under weiss_data")
    if not clb_files:
        print("      nothing to do - are the archives still packed?")
        return {}

    groups = defaultdict(list)
    ok, failed = 0, []
    for path in clb_files:
        grp = group_of(path)
        try:
            with open(path, "rb") as f:
                data = f.read()
            cls = convert(decrypt(data))
            # Lay the class out by the package it actually DECLARES, not by its
            # source path - sys/script/cmn/common.clb declares package "cmn".
            internal = get_class_name(cls)
            out_path = os.path.join(CLASSES_DIR, grp, internal + ".class")
            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            with open(out_path, "wb") as f:
                f.write(cls)
            groups[grp].append(internal)
            ok += 1
        except Exception as e:
            failed.append((os.path.relpath(path, DATA_DIR), repr(e)))

    print(f"      converted {ok}, failed {len(failed)}")
    for name, err in failed[:20]:
        print(f"        FAIL {name}: {err}")
    return groups


def stage_jars(groups):
    """One jar per group. The sys classes go into EVERY zone jar as well, so a
    zone's references to cmn/common and fld/com resolve during decompilation."""
    print(f"[2/3] building {len(groups)} jars")
    os.makedirs(JARS_DIR, exist_ok=True)
    sys_dir = os.path.join(CLASSES_DIR, "sys")
    for grp in sorted(groups):
        jar_path = os.path.join(JARS_DIR, f"{grp}.jar")
        grp_dir = os.path.join(CLASSES_DIR, grp)
        with zipfile.ZipFile(jar_path, "w", zipfile.ZIP_DEFLATED) as z:
            for root, _, files in os.walk(grp_dir):
                for fn in files:
                    if not fn.endswith(".class"):
                        continue
                    full = os.path.join(root, fn)
                    z.write(full, os.path.relpath(full, grp_dir).replace("\\", "/"))
            if grp != "sys" and os.path.isdir(sys_dir):
                for root, _, files in os.walk(sys_dir):
                    for fn in files:
                        if not fn.endswith(".class"):
                            continue
                        full = os.path.join(root, fn)
                        z.write(full, os.path.relpath(full, sys_dir).replace("\\", "/"))
        print(f"      {grp}.jar  ({len(groups[grp])} scripts)")


def stage_decompile(groups):
    print(f"[3/3] decompiling with CFR")
    for grp in sorted(groups):
        jar_path = os.path.join(JARS_DIR, f"{grp}.jar")
        out_dir = os.path.join(JAVA_DIR, grp)
        os.makedirs(out_dir, exist_ok=True)
        r = subprocess.run(
            ["java", "-jar", CFR_JAR, jar_path, "--outputdir", out_dir],
            capture_output=True, text=True)
        n = sum(len(f) for _, _, f in os.walk(out_dir))
        status = "ok" if r.returncode == 0 else f"rc={r.returncode}"
        print(f"      {grp}: {n} .java files ({status})")
        if r.returncode != 0:
            print("        " + (r.stderr or "")[:400])


def main():
    if not os.path.isfile(CFR_JAR):
        sys.exit(f"CFR jar not found: {CFR_JAR}")
    for d in (CLASSES_DIR, JARS_DIR, JAVA_DIR):
        if os.path.isdir(d):
            shutil.rmtree(d)
    os.makedirs(CLASSES_DIR, exist_ok=True)

    groups = stage_convert()
    if not groups:
        return
    stage_jars(groups)
    stage_decompile(groups)
    print(f"\ndone -> {JAVA_DIR}")


if __name__ == "__main__":
    main()
