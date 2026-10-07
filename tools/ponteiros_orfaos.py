#!/usr/bin/env python3
"""Lists code pointers in the data of an image that do not land on a known function start
("ponteiros órfãos" means "orphan pointers"), and proposes the ones to declare.

The run time stops with "[FATAL] Call to invalid or unregistered function at guest address X" when
the game calls through a pointer (static initializer tables, atexit lists, vtables) to an address
codegen never declared as a function. Those pointers are already in the image's data, so they can be
found before running: every aligned big-endian word outside the code that points into the code,
compared with the functions codegen generated.

    python3 tools/ponteiros_orfaos.py app/out/imagem/image_82000000.bin app/generated/default
    python3 tools/ponteiros_orfaos.py app/out/imagem/image_88000000.bin \\
        app/generated/xmediafacade_default --toml

Each orphan gets a class:
  declare   it falls in a gap (no generated function covers it) right after a b / blr / bctr, and
            its first word decodes: a function the analysis did not reach. --toml prints only these,
            as lines for overrides.toml.
  inside    it falls inside a generated function: a switch case label, or a catch block that runs in
            its parent's frame. Not a function; never declare it (that would split the function).
  check     it falls in a gap but does not follow a b / blr / bctr, or starts with an invalid word.
            Look at it with tools/desmontar.py.
The import thunks (the gap at the end of the code, patched by the loader) are left out.
Needs capstone (pip install capstone).
"""
import argparse
import bisect
import json
import os
import re
import struct
import sys

import capstone

RE_FUNC = re.compile(r'DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)')
BLR, BCTR = 0x4E800020, 0x4E800420


def measure_functions(gen_dir):
    """{start: size in bytes} of every generated function. Codegen writes one '\\t// <asm>'
    comment per PowerPC instruction, and every instruction takes 4 bytes."""
    sizes = {}
    for name in sorted(f for f in os.listdir(gen_dir) if f.endswith('.cpp')):
        current, count = None, 0
        with open(os.path.join(gen_dir, name), encoding='utf-8', errors='replace') as fh:
            for line in fh:
                if line.startswith('DEFINE_REX_FUNC('):
                    m = RE_FUNC.match(line)
                    if m:
                        current, count = int(m.group(1), 16), 0
                elif current is not None:
                    if line.startswith('\t// '):
                        count += 1
                    elif line.startswith('}'):
                        sizes[current] = 4 * count
                        current = None
    return sizes


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('image', help='image_<base>.bin written by REXGLUE_DUMP_IMAGE')
    p.add_argument('gen', help='generated folder of the module (with codegen.partition.json)')
    p.add_argument('--toml', action='store_true', help='print only the "declare" ones, as TOML lines')
    args = p.parse_args()

    base = int(re.search(r'image_([0-9A-Fa-f]{8})\.bin$', os.path.basename(args.image)).group(1), 16)
    data = open(args.image, 'rb').read()
    word = lambda a: struct.unpack('>I', data[a - base:a - base + 4])[0]
    starts = sorted(int(k, 16) for k in json.load(open(os.path.join(args.gen, 'codegen.partition.json')))['assignments'])
    sizes = measure_functions(args.gen)
    code_lo = starts[0]
    code_hi = max(s + sizes.get(s, 4) for s in starts)
    known = set(starts)
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)

    orphans = {}
    for offset in range(0, len(data) - 3, 4):
        where = base + offset
        if code_lo <= where < code_hi:
            continue
        value = struct.unpack('>I', data[offset:offset + 4])[0]
        if code_lo <= value < code_hi and value % 4 == 0 and value not in known:
            orphans.setdefault(value, []).append(where)

    counts = {}
    for target in sorted(orphans):
        i = bisect.bisect_right(starts, target) - 1
        owner = starts[i] if i >= 0 else None
        inside = owner is not None and target < owner + sizes.get(owner, 0)
        prev = word(target - 4)
        after_end = prev in (BLR, BCTR) or (prev >> 26 == 18 and prev & 3 == 0)
        text = []
        for k in range(4):
            ins = next(md.disasm(struct.pack('>I', word(target + 4 * k)), target + 4 * k), None)
            text.append(f'{ins.mnemonic} {ins.op_str}' if ins else '???')
        kind = 'inside' if inside else ('declare' if after_end and text[0] != '???' else 'check')
        counts[kind] = counts.get(kind, 0) + 1
        refs = ', '.join(f'{r:08X}' for r in orphans[target][:3])
        if args.toml:
            if kind == 'declare':
                print(f'"0x{target:08X}" = {{}}  # {text[0]} ; {text[1]}')
        else:
            print(f'{target:08X}  {kind:<7}  refs: {refs:<28}  {" ; ".join(text)}')
    print(', '.join(f'{v} {k}' for k, v in sorted(counts.items())), file=sys.stderr)


if __name__ == '__main__':
    main()
