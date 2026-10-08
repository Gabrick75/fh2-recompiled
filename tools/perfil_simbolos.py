#!/usr/bin/env python3
"""Symbolizes fh2_perfil.txt (app/src/fh2_perfil.cpp) with addr2line, grouping samples by function.

    python3 tools/perfil_simbolos.py app/out/run/fh2_perfil.txt [N]

Libraries are looked up by the path in the file; pass LIBDIR=dir to read them from another folder (the unstripped
Android libraries, for example)."""
import collections, os, subprocess, sys

path = sys.argv[1]
top = int(sys.argv[2]) if len(sys.argv) > 2 else 40
libdir = os.environ.get("LIBDIR")
by_lib = collections.defaultdict(list)
total = 0
for line in open(path):
    if line.startswith("#"):
        print(line.strip())
        continue
    n, lib, off = line.split()
    n = int(n)
    total += n
    by_lib[lib].append((int(off, 16), n))
functions = collections.Counter()
for lib, rows in by_lib.items():
    binary = os.path.join(libdir, os.path.basename(lib)) if libdir else lib
    if lib == "?" or not os.path.exists(binary):
        for off, n in rows:
            functions[f"{os.path.basename(lib)} ?"] += n
        continue
    out = subprocess.run(["addr2line", "-f", "-C", "-e", binary] + [hex(o) for o, _ in rows],
                         capture_output=True, text=True).stdout.splitlines()
    for i, (off, n) in enumerate(rows):
        name = out[2 * i] if 2 * i < len(out) else "?"
        functions[f"{os.path.basename(lib)} {name}"] += n
for name, n in functions.most_common(top):
    print(f"{100.0 * n / total:5.1f}%  {name[:150]}")
