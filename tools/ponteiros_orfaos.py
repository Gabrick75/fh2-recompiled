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

Addresses built in the code with lis + addi / ori (callbacks passed to threads and jobs) are
collected too. Each orphan gets a class:
  declare   it follows a blr / bctr or a tail call (a "b" out of the function), its first word
            decodes, and nothing inside a function branches to it: it is in a gap, or the analysis
            swallowed it into the previous function. --toml prints only these, for overrides.toml.
  inside    a generated function has a loc_ label there (a switch case label or a catch block that
            runs in its parent's frame). Not a function; declaring it would split the function.
  check     anything else. Look at it with tools/desmontar.py.
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
RE_LABEL = re.compile(r'loc_([0-9A-Fa-f]{8}):')
BLR, BCTR = 0x4E800020, 0x4E800420


def measure_functions(gen_dir):
    """({start: size in bytes} of every generated function, set of loc_ label addresses).
    Codegen writes one '\\t// <asm>' comment per PowerPC instruction (4 bytes each), and a
    'loc_XXXXXXXX:' label at every address something inside the function branches to."""
    sizes, labels = {}, set()
    for name in sorted(f for f in os.listdir(gen_dir) if f.endswith('.cpp')):
        current, count = None, 0
        with open(os.path.join(gen_dir, name), encoding='utf-8', errors='replace') as fh:
            for line in fh:
                if line.startswith('DEFINE_REX_FUNC('):
                    m = RE_FUNC.match(line)
                    if m:
                        current, count = int(m.group(1), 16), 0
                elif line.startswith('loc_'):
                    m = RE_LABEL.match(line)
                    if m:
                        labels.add(int(m.group(1), 16))
                elif current is not None:
                    if line.startswith('\t// '):
                        count += 1
                    elif line.startswith('}'):
                        sizes[current] = 4 * count
                        current = None
    return sizes, labels


def branch_target(word, pc):
    """Target of a direct b / bc at pc (without the link bit), or None."""
    w = word(pc)
    op = w >> 26
    if op == 18 and w & 2 == 0:
        disp = w & 0x03FFFFFC
        return pc + (disp - 0x04000000 if disp & 0x02000000 else disp)
    if op == 16 and w & 2 == 0:
        disp = w & 0xFFFC
        return pc + (disp - 0x10000 if disp & 0x8000 else disp)
    return None


def split_is_safe(word, start, cut, end, labels):
    """Declaring a function at cut, inside the generated function [start, end), splits it in two.
    That is only safe if no branch crosses the cut in either direction (before -> after, or a loop
    or jump back from after -> before), and every loc_ label in [cut, end) is the target of a
    direct branch from [cut, end) (otherwise it is reached from a jump table or from before)."""
    for pc in range(start, cut, 4):
        t = branch_target(word, pc)
        if t is not None and cut <= t < end:
            return False
    local = {branch_target(word, pc) for pc in range(cut, end, 4)}
    if any(t is not None and start <= t < cut for t in local):
        return False
    return all(l in local for l in labels if cut <= l < end)


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
    sizes, labels = measure_functions(args.gen)
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

    # Addresses built in the code: lis rD, hi followed (within 8 instructions) by addi / ori rX, rD, lo.
    # This is how callbacks are passed (thread entry points, job functions, comparators).
    for offset in range(code_lo - base, code_hi - base - 4, 4):
        w = struct.unpack('>I', data[offset:offset + 4])[0]
        if w >> 26 != 15 or (w >> 16) & 31 != 0:
            continue
        rd, hi = (w >> 21) & 31, w & 0xFFFF
        for k in range(1, 9):
            w2 = struct.unpack('>I', data[offset + 4 * k:offset + 4 * k + 4])[0]
            if w2 >> 26 in (14, 24) and (w2 >> 16) & 31 == rd:
                lo = w2 & 0xFFFF
                value = ((hi << 16) + (lo - 0x10000 if w2 >> 26 == 14 and lo & 0x8000 else lo)) & 0xFFFFFFFF
                if code_lo <= value < code_hi and value % 4 == 0 and value not in known:
                    orphans.setdefault(value, []).append(base + offset)
                break
            if (w2 >> 21) & 31 == rd:
                break  # rD overwritten

    counts = {}
    for target in sorted(orphans):
        i = bisect.bisect_right(starts, target) - 1
        owner = starts[i] if i >= 0 else None
        owner_end = owner + sizes.get(owner, 0) if owner is not None else 0
        inside = target < owner_end
        prev = word(target - 4)
        if prev >> 26 == 18 and prev & 3 == 0:
            disp = prev & 0x03FFFFFC
            dest = target - 4 + (disp - 0x04000000 if disp & 0x02000000 else disp)
            after_end = not (inside and owner <= dest < owner_end)  # a tail call, not a local jump
        else:
            after_end = prev in (BLR, BCTR)
        text = []
        for k in range(4):
            ins = next(md.disasm(struct.pack('>I', word(target + 4 * k)), target + 4 * k), None)
            text.append(f'{ins.mnemonic} {ins.op_str}' if ins else '???')
        if inside and target in labels:
            kind = 'inside'   # something in the function branches here: case label or catch block
        elif inside and not split_is_safe(word, owner, target, owner_end, labels):
            kind = 'check'    # the part before it jumps past it, or a label after it has no direct source
        elif after_end and text[0] != '???':
            kind = 'declare'  # in a gap, or swallowed by the previous function after its last exit
        else:
            kind = 'check'
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
