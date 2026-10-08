"""Lists the vertex fetches of a vertex shader container next to its declared vertex elements ("buscas de vértices"):
address, fetch constant, offset, stride, mini fetch, format. For the translator's "FETCH de vertices sin elemento
declarado" cases.

    python3 tools/xenos/buscas_vertices.py <container .bin>"""
import struct, sys
U = ["Position", "BlendWeight", "BlendIndices", "Normal", "PSize", "TexCoord", "Tangent", "Binormal", "TessFactor",
     "PositionT", "Color", "Fog", "Depth", "Sample"]
b = open(sys.argv[1], 'rb').read()
flags, v, ph, _, ct, dt, so = struct.unpack('>7I', b[:28])
poff, size = struct.unpack('>II', b[so:so+8])
f18, cnt, f20 = struct.unpack('>3I', b[so+24:so+36]); base = so + 36
print('elementos:')
for i in range(cnt):
    w, = struct.unpack('>I', b[base+4*(f18+i):base+4*(f18+i)+4])
    print('  endereco %3d %s%d' % (w & 0xFFF, U[(w >> 12) & 0xF], (w >> 16) & 0xF))
code = b[v+poff:v+poff+size]; w = struct.unpack('>%dI' % (len(code)//4), code)
execs = []; i = 0; first = None
while i + 2 < len(w):
    a, c, d = w[i], w[i+1], w[i+2]
    for cf in (a | ((c & 0xFFFF) << 32), (c >> 16) | (d << 16)):
        op = (cf >> 44) & 0xF
        if op in (1, 2, 3, 4, 5, 6):
            addr, count, seq = cf & 0xFFF, (cf >> 12) & 0x7, (cf >> 16) & 0xFFF
            execs.append((addr, count, seq)); first = addr if first is None else min(first, addr)
    i += 3
    if first is not None and i >= first * 3: break
print('buscas de vertices:')
for addr, count, seq in execs:
    for k in range(count):
        if (seq >> (2*k)) & 1:
            a = addr + k; x, y, z = w[3*a], w[3*a+1], w[3*a+2]
            opcode = x & 0x1F
            if opcode != 0: continue  # 0 = vertex fetch
            const = (x >> 20) & 0x1F; sel = (x >> 25) & 3
            fmt = (y >> 16) & 0x3F; mini = (y >> 30) & 1
            stride = z & 0xFF; off = (z >> 8) & 0x7FFFFF
            if off & 0x400000: off -= 0x800000
            print('  endereco %3d const %2d sel %d offset %3d stride %3d mini %d formato %2d' % (a, const, sel, off, stride, mini, fmt))
