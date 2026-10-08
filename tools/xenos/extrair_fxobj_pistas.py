"""Unpacks the .fxobj files inside FH2's track zips ("extrair fxobj das pistas"). Those zips (up to 3 GB) have no
local headers: an entry's data starts at its header_offset. Method 21 is XMemCompress LZX: frames of
[0xFF, uncompressed BE16, compressed BE16] or [compressed BE16] (32 KB uncompressed), whose payloads concatenated are
one LZX stream with a 128 KB window (lzx_xmem). Identical files are written once.

    python3 tools/xenos/extrair_fxobj_pistas.py <game_root> <new output folder> <lzx_xmem binary>"""
import glob, hashlib, os, struct, subprocess, sys, tempfile
raiz, dest, lzx = sys.argv[1:4]
os.makedirs(dest)
seen = set(); n = dup = 0
tmp = tempfile.mkdtemp()
for f in sorted(glob.glob(os.path.join(raiz, 'media/tracks/**/*.zip'), recursive=True)):
    import zipfile
    z = zipfile.ZipFile(f); fp = open(f, 'rb')
    for i in z.infolist():
        if not i.filename.lower().endswith('.fxobj'):
            continue
        fp.seek(i.header_offset); d = fp.read(i.compress_size)
        if i.compress_type == 0:
            data = d
        else:
            out = bytearray(); p = 0
            while p < len(d):
                if d[p] == 0xFF:
                    cs = struct.unpack('>H', d[p+3:p+5])[0]; p += 5
                else:
                    cs = struct.unpack('>H', d[p:p+2])[0]; p += 2
                if cs == 0:
                    break
                out += d[p:p+cs]; p += cs
            open(os.path.join(tmp, 'in'), 'wb').write(out)
            r = subprocess.run([lzx, os.path.join(tmp, 'in'), str(i.file_size), os.path.join(tmp, 'out')],
                               capture_output=True)
            if r.returncode:
                print('falhou:', f, i.filename); continue
            data = open(os.path.join(tmp, 'out'), 'rb').read()
        h = hashlib.sha1(data).hexdigest()
        if h in seen:
            dup += 1; continue
        seen.add(h); n += 1
        open(os.path.join(dest, '%03d_%s' % (n, os.path.basename(i.filename))), 'wb').write(data)
print('fxobj de pistas distintos:', n, 'repetidos:', dup)
