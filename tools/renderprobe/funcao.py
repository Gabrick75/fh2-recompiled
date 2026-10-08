"""Prints the PowerPC disassembly (the comments of the generated code) of guest functions ("função").

    python3 tools/renderprobe/funcao.py sub_82B70280 [sub_X ...]"""
import glob, os, re, sys
GEN = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'app', 'generated', 'default')
want = set(sys.argv[1:])
for f in sorted(glob.glob(os.path.join(GEN, 'fh2_recomp.*.cpp'))):
    cur = None
    for l in open(f, encoding='utf-8', errors='replace'):
        m = re.match(r'(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL|PPC_FUNC)\((?:__imp__)?(sub_[0-9A-F]+)\)', l)
        if m:
            cur = m.group(1)
            if cur in want:
                print('=== ' + cur)
            continue
        if cur in want and l.lstrip().startswith('//'):
            print(l.strip()[3:])
