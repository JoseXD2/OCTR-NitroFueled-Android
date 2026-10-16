#!/usr/bin/env python3
"""Generate stub shared-library sources from AOSP *.map.txt files (like the NDK's ndkstubgen).

usage: mkstubs.py MAP API ARCH OUT_C OUT_VERSION_SCRIPT
"""
import re, sys

mapfile, api, arch, out_c, out_v = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
ARCHES = {"arm", "arm64", "x86", "x86_64", "riscv64"}

def tags_of(comment):
    return comment.split() if comment else []

def api_of(tag_value):
    try:
        return int(tag_value)
    except ValueError:
        # codenames (e.g. "S", "Tiramisu") are all newer than anything we target
        return 10_000

def included(tags, api):
    t = set()
    for tag in tags:
        t.add(tag.split("=")[0])
    if t & {"platform-only", "apex", "llndk", "systemapi", "future", "vndk"} and not "introduced" in t:
        if "apex" in t or "platform-only" in t or "llndk" in t or "systemapi" in t or "future" in t:
            return False
    if "future" in t or "platform-only" in t:
        return False
    tag_arches = t & ARCHES
    if tag_arches and arch not in tag_arches:
        return False
    for tag in tags:
        if "=" in tag:
            k, v = tag.split("=", 1)
            if k == "introduced" or k == f"introduced-{arch}":
                if api_of(v) > api:
                    return False
            if k == "obsoleted" and api_of(v) <= api:
                return False
    return True

versions = []  # (name, base, [(sym, is_var)])
cur = None
skip_version = False
in_cxx = False
text = open(mapfile).read()
for raw in text.splitlines():
    line, _, comment = raw.partition("#")
    line = line.strip()
    tags = tags_of(comment)
    if not line:
        continue
    m = re.match(r"^([A-Za-z0-9_.]+)\s*\{$", line)
    if m:
        cur = [m.group(1), None, []]
        vtags = set(x.split("=")[0] for x in tags)
        skip_version = ("platform-only" in vtags or "apex" in vtags or "llndk" in vtags or
                        "systemapi" in vtags or "future" in vtags or m.group(1).endswith("_PRIVATE") or
                        m.group(1).endswith("_PLATFORM") or not included(tags, api))
        continue
    if line.startswith('extern "C++"'):
        in_cxx = True
        continue
    if in_cxx:
        if line.startswith("}"):
            in_cxx = False
        continue
    m = re.match(r"^\}\s*([A-Za-z0-9_.]*)\s*;$", line)
    if m and cur is not None:
        cur[1] = m.group(1) or None
        if not skip_version:
            versions.append(cur)
        cur = None
        continue
    if line in ("global:", "local:", "local: *;"):
        continue
    if line.startswith("local:") or line == "*;":
        continue
    if cur is None or skip_version:
        continue
    if line.endswith(";"):
        sym = line[:-1].strip()
        if "*" in sym:
            continue
        if not included(tags, api):
            continue
        cur[2].append((sym, "var" in tags or "weak" in tags and False))

with open(out_c, "w") as c:
    seen = set()
    for name, base, syms in versions:
        for sym, is_var in syms:
            if sym in seen:
                continue
            seen.add(sym)
            if is_var:
                c.write(f"__attribute__((visibility(\"default\"))) int {sym} = 0;\n")
            else:
                c.write(f"__attribute__((visibility(\"default\"))) void {sym}(void) {{}}\n")
with open(out_v, "w") as v:
    names = [x[0] for x in versions]
    vseen = set()
    for name, base, syms in versions:
        v.write(name + " {\n  global:\n")
        for sym, _ in syms:
            if sym in vseen: continue
            vseen.add(sym)
            v.write(f"    {sym};\n")
        if name == names[-1] or True:
            pass
        v.write("}" + (f" {base}" if base and base in names else "") + ";\n")
