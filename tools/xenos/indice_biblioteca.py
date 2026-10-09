"""Which container each shader number of the library is ("índice da biblioteca"): the native renderer's logs name
shaders by their position in fh2_shaders.nfsp (VS n752, PS n601); this maps them to the container files, and so to
their HLSL in app/out/shaders_fh2/hlsl.

    python3 tools/xenos/indice_biblioteca.py [n ...]      (no numbers: the whole list)"""
import glob, hashlib, os, struct, sys
RAIZ = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
S = os.path.join(RAIZ, 'app', 'out', 'shaders_fh2')
por_hash = {}
for f in glob.glob(os.path.join(S, 'contenedores', '*.bin')):
    por_hash.setdefault(hashlib.sha1(open(f, 'rb').read()).hexdigest(), os.path.basename(f)[:-4])
d = open(os.path.join(S, 'fh2_shaders.nfsp'), 'rb').read()
cantidad = struct.unpack('<I', d[12:16])[0]
pos = 24
nombres = []
for i in range(cantidad):
    original, palabras = struct.unpack('<II', d[pos:pos + 8])
    pos += 16
    nombres.append(por_hash.get(hashlib.sha1(d[pos:pos + original]).hexdigest(), '?'))
    pos += original + palabras * 4
pedidos = [int(x.lstrip('n')) for x in sys.argv[1:]]
for i, n in enumerate(nombres):
    if not pedidos or i in pedidos:
        print('n%d %s' % (i, n))
