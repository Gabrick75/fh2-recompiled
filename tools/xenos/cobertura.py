"""Which shaders the game really loaded (the emulator's .xsh shader storage) are in the translated library.
Pixel shaders: exact microcode. Vertex shaders: the D3D library patches their vertex fetches when it uses them, so
they are compared with the dwords of their fetch instructions (found by decoding the control flow) left out.
The storage keeps the microcode in the Xbox 360's byte order (big-endian), like the containers.

    python3 tools/xenos/cobertura.py <.xsh> <containers folder> <spirv folder>"""
import glob, os, struct, sys, collections
xsh, conts, spirv = sys.argv[1], sys.argv[2], sys.argv[3]

def ucode_of_container(b):
    flags, v, ph, c, ct, dt, so = struct.unpack('>7I', b[:28])
    poff, size = struct.unpack('>II', b[so:so+8])
    code = b[v+poff:v+poff+size]
    return list(struct.unpack('>%dI' % (len(code)//4), code))

def fetch_mask(words):
    """Indices of the dwords of fetch instructions, from the control flow exec clauses."""
    mask = set()
    # control flow: pairs of 48-bit instructions packed in 3 dwords; ends at the first ALU/fetch address.
    n = len(words); i = 0; first_ins = None; execs = []
    while i + 2 < n:
        w0, w1, w2 = words[i], words[i+1], words[i+2]
        for cf in ((w0 | ((w1 & 0xFFFF) << 32)), ((w1 >> 16) | (w2 << 16))):
            op = (cf >> 44) & 0xF
            if op in (1, 2, 3, 4, 5, 6):  # exec, exec_end, cond_exec...
                addr = cf & 0xFFF; count = (cf >> 12) & 0x7; seq = (cf >> 16) & 0xFFF
                execs.append((addr, count, seq))
                if first_ins is None or addr < first_ins: first_ins = addr
        i += 3
        if first_ins is not None and i >= first_ins * 3: break
    for addr, count, seq in execs:
        for k in range(count):
            if (seq >> (2*k)) & 1:  # fetch
                base = (addr + k) * 3
                mask.update((base, base+1, base+2))
    return mask

lib_ps, lib_vs, ok_ps, ok_vs = {}, collections.defaultdict(list), set(), set()
for f in glob.glob(os.path.join(conts, '*.bin')):
    name = os.path.basename(f)[:-4]
    w = ucode_of_container(open(f, 'rb').read())
    translated = os.path.exists(os.path.join(spirv, name + '.spv'))
    if name[0] == 'p':
        lib_ps[tuple(w)] = (name, translated)
    else:
        m = fetch_mask(w)
        lib_vs[(len(w), tuple(x for j, x in enumerate(w) if j not in m))].append((name, translated))

d = open(xsh, 'rb').read(); pos = 8
seen = {0: set(), 1: set()}
res = collections.Counter(); miss = []
while pos + 12 <= len(d):
    h, info = struct.unpack('<QI', d[pos:pos+12]); cnt = info & 0x7FFFFFFF; typ = info >> 31
    words = list(struct.unpack('>%dI' % cnt, d[pos+12:pos+12+cnt*4])); pos += 12 + cnt*4
    if h in seen[typ]: continue
    seen[typ].add(h)
    # xenos::ShaderType: 0 vertex, 1 pixel
    if typ == 1:
        e = lib_ps.get(tuple(words))
        res['ps total'] += 1
        if e: res['ps achado'] += 1; res['ps achado e traduzido'] += e[1]
        else: miss.append(('ps', cnt))
    else:
        m = fetch_mask(words)
        e = lib_vs.get((len(words), tuple(x for j, x in enumerate(words) if j not in m)))
        res['vs total'] += 1
        if e: res['vs achado'] += 1; res['vs achado e traduzido'] += any(t for _, t in e)
        else: miss.append(('vs', cnt))
for k in sorted(res): print('%-24s %d' % (k, res[k]))
print('faltando (tipo, dwords):', collections.Counter(miss).most_common(8))
