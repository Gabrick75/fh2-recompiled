"""Call graph of the recompiled game from app/generated/default/*.cpp.

Every guest function is a `DEFINE_REX_FUNC(sub_XXXXXXXX)` and every direct
call a `sub_XXXXXXXX(ctx, base)` or `__imp__Name(ctx, base)` (kernel import).
Indirect calls (bctrl through function pointers / vtables) are counted per
function but have no target.

    python callgraph.py            writes callgraph.json next to this script
"""
import glob
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "..", "..", "app", "generated", "default")

re_def = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
re_call = re.compile(r"\b(sub_[0-9A-F]{8}|__imp__\w+)\(ctx, base\)")
re_comment = re.compile(r"^\s*// (\w+)")

funcs = {}
cur = None
for path in sorted(glob.glob(os.path.join(GEN, "fh2_recomp.*.cpp"))):
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re_def.match(line)
            if m:
                cur = {"calls": {}, "imports": {}, "insns": 0, "bctrl": 0, "file": os.path.basename(path)}
                funcs[m.group(1)] = cur
                continue
            if cur is None:
                continue
            c = re_comment.match(line)
            if c:
                cur["insns"] += 1
                if c.group(1) in ("bctrl", "bcctrl"):
                    cur["bctrl"] += 1
                continue
            for callee in re_call.findall(line):
                d = cur["imports"] if callee.startswith("__imp__") else cur["calls"]
                key = callee[7:] if callee.startswith("__imp__") else callee
                d[key] = d.get(key, 0) + 1

out = os.path.join(HERE, "callgraph.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(funcs, f)
print(f"{len(funcs)} functions, {sum(len(v['calls']) for v in funcs.values())} call edges -> {out}")
