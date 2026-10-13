#!/usr/bin/env python3
"""Rewrites ShiftGlue's generated generated/default/fh2_register.cpp (one function with ~470,000 SetFunction calls,
which stalled the NDK compiler for over an hour) as a table of the same pairs, in the same order, walked by a loop.
Run after every codegen, before the Android build:  python3 tabela_registro.py generated/default/fh2_register.cpp"""
import re
import sys

p = sys.argv[1]
s = open(p).read()
pares = re.findall(r'registrar->SetFunction\((0x[0-9A-Fa-f]+), (\w+)\);', s)
if not pares:
    sys.exit('no SetFunction calls (already a table?)')
out = ['// Rewritten by tabela_registro.py: the generated registration as a table.',
       '#include "fh2_init.h"', '#include <rex/system/function_dispatcher.h>', '', 'namespace {',
       'struct Fh2Registro { uint32_t guest; ::PPCFunc* host; };', 'const Fh2Registro kFh2Registros[] = {']
out += ['  {%s, %s},' % par for par in pares]
out += ['};', '}  // namespace', '', 'void fh2_RegisterFunctions(rex::runtime::IModuleRegistrar* registrar) {',
        '  for (const Fh2Registro& r : kFh2Registros) registrar->SetFunction(r.guest, r.host);', '}', '']
open(p, 'w').write('\n'.join(out))
print(len(pares), 'pairs')
