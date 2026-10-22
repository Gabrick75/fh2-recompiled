# Reads before writes of r14-r31, f14-f31, v-registers, cr, ctr and xer in the generated code: candidates for
# share_registers once those registers are C++ locals (non_volatile_as_local and the rest).
#   python3 tools/lee_antes.py app/generated/default [--ayudantes 82C01000-82C02800]
# --ayudantes: the guest range of the __savegprlr/__restgprlr/__savefpr/__savevmx helpers. A function that calls
# one (its "// bl" comment is still there) saved its registers in its prologue, so what it reads before writing is a
# loop, not its caller's value: it is not listed, and it must not get share_registers (see app/overrides.toml).
import re,glob,sys
gen=sys.argv[1]
ayudantes=None
if '--ayudantes' in sys.argv:
    lo,hi=sys.argv[sys.argv.index('--ayudantes')+1].split('-'); ayudantes=(int(lo,16),int(hi,16))
bl_re=re.compile(r'^\s*// bl 0x([0-9a-f]+)')
fn_re=re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)')
tok=re.compile(r'(?<![.\w])(r(?:1[4-9]|2[0-9]|3[01])|f(?:1[4-9]|2[0-9]|3[01])|v(?:1[4-9]|2[0-9]|3[01]|6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])|cr[0-7]|ctr|xer)\b')
asg=re.compile(r'^([A-Za-z0-9_]+)(?:\.[A-Za-z0-9_\[\]]+)*\s*=[^=]')
res={}
for f in sorted(glob.glob(gen+'/nfsmw_recomp.*.cpp')+glob.glob(gen+'/fh2_recomp.*.cpp')):
    cur=None
    for ln in open(f,encoding='utf-8',errors='ignore'):
        m=fn_re.match(ln)
        if m: cur=m.group(1);written=set();bad={};prologo=False;continue
        if cur is None: continue
        if ln.startswith('}'):
            if bad and not prologo: res[cur]=bad
            cur=None;continue
        mb=bl_re.match(ln)
        if mb and ayudantes and ayudantes[0]<=int(mb.group(1),16)<ayudantes[1]: prologo=True
        s=ln.strip()
        if not s or s.startswith('//') or s.startswith('PPC') or s.startswith('uint') or s.startswith('REX_FUNC_PROLOGUE'): continue
        if s.startswith('REX_STORE') and re.search(r',\s*(r|f)\d+\.(u64|f64)\);$',s) and ('ctx.r1.' in s): continue  # guardado de prologo
        s2=re.sub(r'compare<[^>]*>\([^;]*,\s*xer\)','',s)
        s2=re.sub(r'cr\d\.so = xer\.so;','',s2)
        mc=re.match(r'^(cr[0-7])\.(?:compare|setFromMask)',s)
        if mc:
            for r in tok.findall(re.sub(r',\s*xer\)',')',s[len(mc.group(1)):])):
                if r not in written and r not in bad: bad[r]=s[:100]
            written.add(mc.group(1)); continue
        ms=re.match(r'^simde_mm_store\w*\(\(?(?:simde__m128i\*\))?(v\d+)\.\w+,(.*)$',s2)
        if ms:
            rhs=ms.group(2)
            for r in tok.findall(rhs):
                if r not in written and r not in bad: bad[r]=s[:100]
            written.add(ms.group(1)); continue
        m2=asg.match(s2); lhs=None; rhs=s2
        if m2: lhs=m2.group(1); rhs=s2[m2.end()-1:]
        for r in tok.findall(rhs):
            if r not in written and r not in bad: bad[r]=s[:100]
        if lhs and tok.fullmatch(lhs): written.add(lhs)
        # calls through a function pointer to simd that write by reference: ignore
for n in sorted(res): print(n, ' '.join(sorted(res[n])), '|', list(res[n].values())[0])
print('TOTAL',len(res),file=sys.stderr)
