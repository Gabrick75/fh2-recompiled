#!/usr/bin/env python3
"""Lists code pointers in the data of an image that do not land on a known function start
("ponteiros órfãos" means "orphan pointers").

The run time stops with "[FATAL] Call to invalid or unregistered function at guest address X" when
the game calls through a pointer (static initializer tables, atexit lists, vtables) to an address
codegen never declared as a function. Those pointers are already in the image's data, so they can be
found before running: every aligned big-endian word outside the code range that points into the
code, compared with the function starts in codegen.partition.json.

    python3 tools/ponteiros_orfaos.py app/out/imagem/image_82000000.bin \\
        app/generated/default/codegen.partition.json

For each orphan it prints the word before it (a function that starts there normally follows a
"b" or "blr" of the previous one) and its first instructions. Check each one with
tools/desmontar.py before declaring it in overrides.toml: a word in the data can look like a code
address by chance, and declaring a function in the middle of another one splits it.
"""
import json
import re
import struct
import sys
import os

import capstone


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    image_path, partition_path = sys.argv[1:]
    base = int(re.search(r'image_([0-9A-Fa-f]{8})\.bin$', os.path.basename(image_path)).group(1), 16)
    data = open(image_path, 'rb').read()
    starts = sorted(int(k, 16) for k in json.load(open(partition_path))['assignments'])
    code_lo, code_hi = starts[0], starts[-1] + 0x10000
    word = lambda a: struct.unpack('>I', data[a - base:a - base + 4])[0]
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

    for target in sorted(orphans):
        prev = word(target - 4)
        ends = prev in (0x4E800020, 0x4E800420) or (prev >> 26 == 18 and prev & 3 == 0)
        text = []
        for i in range(4):
            ins = next(md.disasm(struct.pack('>I', word(target + 4 * i)), target + 4 * i), None)
            text.append(f'{ins.mnemonic} {ins.op_str}' if ins else '???')
        refs = ', '.join(f'{r:08X}' for r in orphans[target][:3])
        print(f'{target:08X}  after b/blr: {"yes" if ends else "NO "}  refs: {refs:<28}  {" ; ".join(text)}')
    print(f'{len(orphans)} orphan code pointers', file=sys.stderr)


if __name__ == '__main__':
    main()
