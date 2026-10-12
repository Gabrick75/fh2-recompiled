"""Aligns the draw sequence of an emulated frame trace ([traco] DRAW) with a native one ([traza] dibujo) by
(primitive, count) and prints the runs the native frame lacks, with the emulated shaders and targets."""
import collections, difflib, re, sys

emu, nat = [], []
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r'\[traco\]\s+(\d+) DRAW prim (\d+) n (\d+) VS (\w+) PS (\w+) pitch (\d+)', l)
    if m:
        emu.append((int(m.group(2)), int(m.group(3)), m.group(4), m.group(5), int(m.group(6)), l.strip()[60:300]))
for l in open(sys.argv[2], errors='replace'):
    m = re.search(r'\[traza\] dibujo VS n(-?\d+) PS n(-?\d+) tipo (\d+) cuenta (\d+) sup (\w+)', l)
    if m:
        nat.append((int(m.group(3)), int(m.group(4)), m.group(1), m.group(2), int(m.group(5), 16) & 0x3FFF, l.strip()[60:300]))
ke = [(d[0], d[1]) for d in emu]
kn = [(d[0], d[1]) for d in nat]
print('emulated', len(emu), 'native', len(nat))
sm = difflib.SequenceMatcher(None, ke, kn, autojunk=False)
faltan = collections.Counter()
for op, i1, i2, j1, j2 in sm.get_opcodes():
    if op in ('delete', 'replace'):
        print('--- %s emu[%d:%d] nat[%d:%d]' % (op, i1, i2, j1, j2))
        for d in emu[i1:i2][:12]:
            print('  E', d[5])
        for d in nat[j1:j2][:6]:
            print('  N', d[5])
        for d in emu[i1:i2]:
            faltan[(d[2], d[3])] += 1
print('missing emulated VS/PS pairs:')
for k, v in faltan.most_common(30):
    print(v, k)
