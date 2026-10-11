"""Wraps the D3D library's own shaders, loose microcode in FH2's executable, in synthetic 2008 containers ("D3D
internos"), as NFS Carbon's tools/biblioteca_shaders_carbon.mjs does for that game. The microcode comes from the
native renderer's dump of what its library lacks (--fh2_nativo_volcar_sueltos=<folder>: {v|p}_<hash>_<words>.bin,
big-endian words); only the small ones (up to 64 words) are the D3D's own, the rest are the game's patched copies.

- No constant table: the translator reads the constants straight from the registers (FH2_RECOMP).
- Vertex shader: one vertex element per vertex fetch (the first POSITION0, then TEXCOORD0..), and interpolators
  TEXCOORD0-3 in registers 0-3, whichever it exports.
- Pixel shader: interpolators TEXCOORD0-3 in registers 0-3 (what the D3D vertex shaders export) and COLOR0 out.
  The pixel position register is left out (fieldC 4, as in the game's own containers): with it at register 0 the
  translator put the pixel's screen position in r0, and the D3D clear shader (oC0 = r0) wrote green and red.

    python3 tools/xenos/d3d_internos.py <dump folder> <containers folder>"""
import glob, os, struct, sys

def fetches(words):
    """Addresses of the vertex fetch instructions, from the control flow's exec clauses."""
    execs, i, first = [], 0, None
    while i + 2 < len(words):
        a, c, d = words[i], words[i + 1], words[i + 2]
        for cf in (a | ((c & 0xFFFF) << 32), (c >> 16) | (d << 16)):
            if ((cf >> 44) & 0xF) in (1, 2, 3, 4, 5, 6):
                addr, count, seq = cf & 0xFFF, (cf >> 12) & 0x7, (cf >> 16) & 0xFFF
                execs.append((addr, count, seq))
                first = addr if first is None else min(first, addr)
        i += 3
        if first is not None and i >= first * 3:
            break
    out = []
    for addr, count, seq in execs:
        for k in range(count):
            a = addr + k
            if (seq >> (2 * k)) & 1 and 3 * a < len(words) and (words[3 * a] & 0x1F) == 0:
                out.append(a)
    return sorted(set(out))

def contenedor(words, vertices):
    w = [0] * 9
    w[0] = 0x102A1101 if vertices else 0x102A1100
    w[4] = 0          # no constant table
    w[6] = 36         # shader struct right after the header
    sh = [0, len(words) * 4, 0, 0, 0]
    if vertices:
        elementos = fetches(words)
        usos = [(0, 0)] + [(5, i) for i in range(len(elementos))]  # POSITION0, TEXCOORD0..
        interp = [(i << 8) | (5 << 4) | i for i in range(4)]       # register i -> TEXCOORD i
        sh.append(len(interp) << 5)
        sh += [0, len(elementos), 0]
        sh += [a | (usos[n][0] << 12) | (usos[n][1] << 16) for n, a in enumerate(elementos)]
        sh += interp
    else:
        sh[3] = 4          # fieldC: no pixel position register (as the game's own pixel shaders)
        interp = [(i << 8) | (5 << 4) | i for i in range(4)]       # register i <- TEXCOORD i
        sh.append(len(interp) << 5)
        sh += [0, 1]       # field18, outputs: COLOR0
        sh += interp
    w += sh
    virtual = len(w) * 4
    w[1] = virtual
    w[2] = len(words) * 4
    return b''.join(struct.pack('>I', x & 0xFFFFFFFF) for x in w) + b''.join(struct.pack('>I', x) for x in words)

entrada, salida = sys.argv[1], sys.argv[2]
n = 0
for f in sorted(glob.glob(os.path.join(entrada, '*.bin'))):
    nombre = os.path.basename(f)[:-4]
    tipo, huella, palabras = nombre.split('_')
    if int(palabras) > 64:
        continue
    d = open(f, 'rb').read()
    words = list(struct.unpack('>%dI' % (len(d) // 4), d))
    open(os.path.join(salida, '%s_d3d_%s.bin' % (tipo, huella)), 'wb').write(contenedor(words, tipo == 'v'))
    n += 1
print('D3D internos envueltos:', n)
