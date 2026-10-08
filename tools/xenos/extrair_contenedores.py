"""Extracts the Xenos shader containers (2008 format, 0x102A11xx) from FH2's .fxobj files ("extrair contêineres"):
the loose ones in media/shaders and the track ones unpacked by extrair_fxobj_pistas.py. One .bin per container (virtual
part followed by physical part), named p_/v_ + a counter in scan order, plus procedencia.tsv with where each came from.

    python3 tools/xenos/extrair_contenedores.py <game_root> <new output folder> <folder of unpacked track .fxobj>
        [executable images...]

The executable images (the game's modules as loaded, written with --xex_despejar_imagem) hold the shaders of the
D3D library and of the effects the code builds itself."""
import glob, os, struct, sys
raiz, saida = sys.argv[1], sys.argv[2]
os.makedirs(saida)
idx = open(os.path.join(saida, 'procedencia.tsv'), 'w')
idx.write('archivo\torigen\tdesplazamiento\tvirtual\tfisico\n')
total = rej = 0
for f in (sorted(glob.glob(os.path.join(raiz, 'media/shaders/**/*.fxobj'), recursive=True)) +
          sorted(glob.glob(os.path.join(sys.argv[3], '*.fxobj'))) + sys.argv[4:]):
    d = open(f, 'rb').read(); n = len(d); i = 0
    while True:
        i = d.find(b'\x10\x2A\x11', i)
        if i < 0 or i + 36 > n: break
        if d[i+3] > 1: i += 1; continue
        flags, v, ph, c, ct, dt, so = struct.unpack('>7I', d[i:i+28])
        if not (24 < v and v + ph <= n - i and ph and v + ph <= 1 << 20 and ct < v and so < v):
            rej += 1; i += 1; continue
        nome = '%s_%06d.bin' % ('v' if d[i+3] else 'p', total)
        open(os.path.join(saida, nome), 'wb').write(d[i:i+v+ph])
        idx.write('%s\t%s\t%d\t%d\t%d\n' % (nome, os.path.relpath(f, raiz), i, v, ph))
        total += 1; i += v + ph
print('contenedores:', total, 'rechazados:', rej)
