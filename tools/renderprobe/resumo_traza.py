"""Summarizes a native renderer frame trace (--nfsmw_native_diag_fotograma_s; the "[traza]" log lines with the
prefix cut): consecutive draws grouped by render target state, with the copies between them ("resumo da traça").

    grep "\\[traza\\]" fh2.log | cut -c60- > traza.txt; python3 tools/renderprobe/resumo_traza.py traza.txt"""
import re, sys, collections
grupo = None; n = 0; tipos = collections.Counter(); ps = collections.Counter(); sem = 0
def fechar():
    if grupo and n:
        print('  %5d draws %-70s PS: %s %s' % (n, grupo, ' '.join('%s:%d' % kv for kv in ps.most_common(4)),
              ('(sem VS/PS: %d)' % sem) if sem else ''))
for l in open(sys.argv[1], errors='replace'):
    if l.startswith('ibujo'):
        m = re.search(r'sup (\S+) rt0 (\S+) rt1 (\S+) mascara (\S+).* prof (\S+) stencil \S+ modo (\S+)', l)
        if not m: continue
        clave = 'sup %s rt0 %s rt1 %s masc %s prof %s modo %s' % m.groups()
        if clave != grupo:
            fechar(); grupo = clave; n = 0; ps = collections.Counter(); sem = 0
        n += 1
        p = re.search(r'VS (n-?\d+) PS (n-?\d+)', l)
        if p:
            ps[p.group(2)] += 1
            if p.group(1) == 'n-1' or p.group(2) == 'n-1': sem += 1
    elif l.startswith('opia'):
        fechar(); grupo = None; n = 0
        m = re.search(r'control (\S+) destino (\S+) info (\S+) pitch (\S+) sup (\S+) rt0 (\S+)', l)
        if m: print('COPIA control %s -> %s info %s pitch %s (sup %s rt0 %s)' % m.groups())
fechar()
