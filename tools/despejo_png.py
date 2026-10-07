#!/usr/bin/env python3
"""Turns the resolve destinations dumped by the GPU frame trace into PNG images ("despejo" means "dump").

Run the game with a frame trace and resolve readback, so the resolved data reaches guest memory:
    tools/teste_jogabilidade.sh --gpu_traco_quadro_s=215 --readback_resolve=fast
The trace writes traco_despejo/<index>_<address>_fmt<format>_<w>x<h>_p<pitch>_e<endian>.bin (raw,
tiled guest memory) in the run folder. Then:
    python3 tools/despejo_png.py app/out/run/traco_despejo

Formats: 6 = k_8_8_8_8, 7 = k_2_10_10_10 (unorm), 36 = k_32_FLOAT (shown scaled to its maximum).
Each image also gets its mean and maximum per channel printed, to compare passes by number.
"""
import os
import re
import sys

import numpy as np
from PIL import Image


def tiled_offsets(width, height, pitch, bpp_log2):
    """Byte offset of every (y, x) in a Xenos 2D-tiled surface (texture_util::GetTiledOffset2D)."""
    pitch = (pitch + 31) & ~31
    y, x = np.mgrid[0:height, 0:width].astype(np.int64)
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpp_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpp_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    folder = sys.argv[1]
    pattern = re.compile(r'(\d+)_([0-9A-F]{8})_fmt(\d+)_(\d+)x(\d+)_p(\d+)_e(\d+)\.bin$')
    for name in sorted(os.listdir(folder)):
        m = pattern.match(name)
        if not m:
            continue
        fmt, width, height, pitch, endian = (int(m.group(i)) for i in range(3, 8))
        raw = np.fromfile(os.path.join(folder, name), dtype=np.uint8)
        offsets = tiled_offsets(width, height, pitch, 2)
        if offsets.max() + 4 > raw.size:
            print(f'{name}: dump too small, skipped')
            continue
        texels = np.stack([raw[offsets + i] for i in range(4)], axis=-1).astype(np.uint32)
        # Endian 2 (8in32) leaves each texel big-endian in memory; 0 keeps the host order.
        if endian == 2:
            words = (texels[..., 0] << 24) | (texels[..., 1] << 16) | (texels[..., 2] << 8) | texels[..., 3]
        else:
            words = texels[..., 0] | (texels[..., 1] << 8) | (texels[..., 2] << 16) | (texels[..., 3] << 24)
        if fmt == 6:
            rgb = np.stack([(words >> s) & 0xFF for s in (0, 8, 16)], axis=-1).astype(np.float64) / 255
        elif fmt == 7:
            rgb = np.stack([(words >> s) & 0x3FF for s in (0, 10, 20)], axis=-1).astype(np.float64) / 1023
        elif fmt == 36:
            values = words.astype(np.uint32).view(np.float32).astype(np.float64)
            print(f'{name}: float min {np.nanmin(values):.6g} max {np.nanmax(values):.6g} '
                  f'mean {np.nanmean(values):.6g} NaN {int(np.isnan(values).sum())}')
            peak = np.nanmax(np.abs(values)) or 1.0
            rgb = np.repeat((values / peak)[..., None], 3, axis=-1)
        else:
            continue
        print(f'{name}: mean {rgb.mean(axis=(0, 1)).round(4)} max {rgb.max(axis=(0, 1)).round(4)}')
        image = (np.clip(np.nan_to_num(rgb), 0, 1) * 255).astype(np.uint8)
        Image.fromarray(image, 'RGB').save(os.path.join(folder, name[:-4] + '.png'))


if __name__ == '__main__':
    main()
