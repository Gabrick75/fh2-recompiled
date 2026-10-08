"""Where is the statically linked Xbox D3D library, and what does the game call?

Seed: functions that call graphics kernel imports (Vd*). The library is one
contiguous address block; find it by growing a window around the seeds while
functions keep calling or being called by the block. Then list the entry
points: block functions called from outside the block.

    python d3d_layer.py [lo hi]
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
g = json.load(open(os.path.join(HERE, "callgraph.json")))
addr = {f: int(f[4:], 16) for f in g if f.startswith("sub_")}
by_addr = sorted(addr, key=addr.get)

callers = {}
for f, v in g.items():
    for c in v["calls"]:
        callers.setdefault(c, set()).add(f)

seeds = [f for f, v in g.items() if any(i.startswith("Vd") for i in v["imports"])]
lo = min(addr[s] for s in seeds)
hi = max(addr[s] for s in seeds)
if len(sys.argv) == 3:
    lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
else:
    # Grow: include neighbours that touch the block (call into it or are
    # called only from it).
    changed = True
    while changed:
        changed = False
        inside = {f for f in by_addr if lo <= addr[f] <= hi}
        i_lo = by_addr.index(min(inside, key=addr.get))
        i_hi = by_addr.index(max(inside, key=addr.get))
        for idx in (i_lo - 1, i_hi + 1):
            if not 0 <= idx < len(by_addr):
                continue
            f = by_addr[idx]
            calls_in = any(c in inside for c in g[f]["calls"])
            called_from_in = any(c in inside for c in callers.get(f, ()))
            if calls_in or called_from_in:
                lo, hi = min(lo, addr[f]), max(hi, addr[f])
                changed = True

inside = {f for f in by_addr if lo <= addr[f] <= hi}
print(f"block {lo:08X}-{hi:08X}: {len(inside)} functions, "
      f"{sum(g[f]['insns'] for f in inside)} instructions")
entries = []
for f in sorted(inside, key=addr.get):
    outside = callers.get(f, set()) - inside
    if outside:
        entries.append((f, len(outside), g[f]["insns"]))
print(f"{len(entries)} entry points called from outside the block")
ext_callers = set()
for f in inside:
    ext_callers |= callers.get(f, set()) - inside
print(f"{len(ext_callers)} game functions call into it")
json.dump({"lo": lo, "hi": hi, "entries": entries}, open(os.path.join(HERE, "d3d_layer.json"), "w"))
for f, n, ins in sorted(entries, key=lambda e: -e[1])[:60]:
    imps = ",".join(sorted(g[f]["imports"]))
    print(f"  {f}  callers {n:4d}  insns {ins:5d}  {imps}")
