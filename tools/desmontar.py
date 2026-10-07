#!/usr/bin/env python3
"""Disassembles PowerPC code of a module image dumped by codegen ("desmontar" means "disassemble").

Dump the images first (they are written to the folder given, one image_<base>.bin per module):
    REXGLUE_DUMP_IMAGE=app/out/imagem sdk/out/linux-amd64/rexglue codegen app/fh2_manifest.toml

Then:
    python3 tools/desmontar.py app/out/imagem/image_82000000.bin 0x8305E8F0:20 0x82E83A50

Each address takes an optional ":N" with the number of instructions (16 by default). The base
address is read from the file name. Needs capstone (pip install capstone).
"""
import os
import re
import sys

import capstone


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    image = sys.argv[1]
    match = re.search(r'image_([0-9A-Fa-f]{8})\.bin$', os.path.basename(image))
    if not match:
        sys.exit('the image name must be image_<base in hex>.bin')
    base = int(match.group(1), 16)
    data = open(image, 'rb').read()
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    for spec in sys.argv[2:]:
        address, _, count = spec.partition(':')
        address = int(address, 16)
        print(f'--- {address:08X}')
        for i in range(int(count or 16)):
            pc = address + 4 * i
            word = data[pc - base:pc - base + 4]
            ins = next(md.disasm(word, pc), None)
            text = f'{ins.mnemonic} {ins.op_str}' if ins else '???'
            print(f'{pc:08X}  {word.hex()}  {text}')


if __name__ == '__main__':
    main()
