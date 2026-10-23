#!/usr/bin/env python3
"""Checks the BC -> ETC2/EAC transcoder (app/src/nativo/nfsmw_texturas_etc.h) with an independent decoder.

Builds tools/probar_etc.cpp, runs it, decodes its ETC2/EAC output (full encode and draft) with texture2ddecoder (pip install
texture2ddecoder numpy) and compares it with the BC decode the CPU path used before (bc::Convertir). Fails if
a texel that was transparent in BC1 is not transparent (or the reverse), or if the error (PSNR per half: random
blocks, synthetic picture) is lower than expected for the format.

    python3 tools/probar_etc.py [--cxx clang++]
"""
import argparse
import pathlib
import subprocess
import sys
import tempfile

import numpy as np
import texture2ddecoder

RAIZ = pathlib.Path(__file__).resolve().parent.parent
LADO = 256
# Minimum PSNR (dB) for the random half and the synthetic half. Random BC1 blocks between any two colours are the
# worst case for ETC2; the synthetic picture is closer to game textures.
MINIMOS = {1: (24, 30), 2: (24, 30), 3: (24, 30), 4: (33, 45), 5: (33, 45)}


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--cxx', default='clang++')
    args = p.parse_args()
    fallos = 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp) / 'probar_etc'
        subprocess.run([args.cxx, '-std=c++20', '-O2', '-I', str(RAIZ / 'app/src/nativo'),
                        str(RAIZ / 'tools/probar_etc.cpp'), '-o', str(exe)], check=True)
        subprocess.run([str(exe), tmp], check=True)
        for f, extension in [(f, e) for f in range(1, 6) for e in ('etc', 'borrador')]:
            ref = np.fromfile(pathlib.Path(tmp) / f'bc{f}.ref', dtype=np.uint8)
            etc = (pathlib.Path(tmp) / f'bc{f}.{extension}').read_bytes()
            nombre_f = f'BC{f}' + (' draft' if extension == 'borrador' else '')
            # The draft is shown for a few frames only: a lower bar.
            margen = 6 if extension == 'borrador' else 0
            canales = {4: 1, 5: 2}.get(f, 4)
            ref = ref.reshape(LADO, LADO, canales)
            decodificar = {1: texture2ddecoder.decode_etc2a1, 2: texture2ddecoder.decode_etc2a8,
                           3: texture2ddecoder.decode_etc2a8, 4: texture2ddecoder.decode_eacr,
                           5: texture2ddecoder.decode_eacrg}[f]
            bgra = np.frombuffer(decodificar(etc, LADO, LADO), dtype=np.uint8).reshape(LADO, LADO, 4)
            rgba = bgra[..., [2, 1, 0, 3]]
            salida = rgba[..., :canales]
            mitad = LADO // 2
            if f == 1:
                # Transparent texels are (0, 0, 0, 0) in BC1; colour only counts where opaque.
                alfa_ref, alfa = ref[..., 3] >= 128, salida[..., 3] >= 128
                malos = int(np.count_nonzero(alfa_ref != alfa))
                if malos:
                    print(f'{nombre_f}: {malos} texels with the wrong transparency')
                    fallos += 1
                # The spec decodes a transparent texel as (0, 0, 0, 0); texture2ddecoder keeps its colour.
                ref = np.where(alfa_ref[..., None], ref, 0)
                salida = np.where(alfa[..., None], salida, 0)
            for nombre, filas, minimo in (('random', slice(0, mitad), MINIMOS[f][0] - margen),
                                         ('synthetic', slice(mitad, LADO), MINIMOS[f][1] - margen)):
                color = slice(0, 3) if canales == 4 else slice(0, canales)
                valor = psnr(ref[filas, :, color], salida[filas, :, color])
                linea = f'{nombre_f} {nombre}: colour {valor:.1f} dB'
                if canales == 4:
                    valor_a = psnr(ref[filas, :, 3], salida[filas, :, 3])
                    linea += f', alpha {valor_a:.1f} dB'
                    valor = min(valor, valor_a)
                ok = valor >= minimo
                print(linea + ('' if ok else f'  < {minimo} dB'))
                fallos += not ok
    print('OK' if not fallos else f'{fallos} failures')
    return 1 if fallos else 0


if __name__ == '__main__':
    sys.exit(main())
