#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Finds code gaps with no function assigned in the codegen output.

The problem
-----------
The binary dies with:
    [FATAL] Call to invalid or unregistered function at guest address 0xXXXXXXXX

That happens when something makes an indirect call (through a pointer or vtable) to an
address the analysis did not mark as a function start. The Validate phase does not
detect them: it only checks direct branches (b / bl). Indirect ones only show up at
run time, one at a time.

How it finds them
-----------------
Every PowerPC instruction takes 4 bytes and the codegen emits exactly one comment line
"\\t// <asm>" per instruction. So:

    fin_de_funcion = inicio + 4 * numero_de_comentarios

With the start of each function (codegen.partition.json) and its computed end, every
stretch between the end of one and the start of the next is a gap with no owner.
Gaps that contain real code are the candidates for breaking startup.

Usage:
    python tools\\huecos.py
    python tools\\huecos.py --min 8
    python tools\\huecos.py --comprobar 0x8215FEA8 0x826BE258
"""


# ============================================================================
#  Known limitation - a gap is not always one function
#
#  This tool emits one declaration per gap, at its start address. That
#  assumes each gap contains exactly one function, and it is false.
#
#  A gap is simply space that automatic discovery did not claim. It can
#  contain several small functions in a row, typically tables of 8- and
#  16-byte thunks. By declaring only the start, the Discover phase swallows
#  the whole gap as one single function and the other entry points become
#  unreachable. The symptom is:
#
#     [FATAL] Call to invalid or unregistered function at guest address 0x...
#
#  with an address that falls inside a gap that is already declared.
#
#  Verified: four different crashes fell inside declared gaps, at +8, +24,
#  +64 and +96 bytes from their start. That is why the crash seemed to
#  "move": each fix uncovered the next entry of the same gap.
#
#  Splitting every gap every 8 bytes is not the solution: that would be more
#  than 3000 declarations, and most gaps do contain a single function, so
#  entry points would be declared in the middle of functions.
#
#  What does work:
#    1. Split only the gaps proven to have several entries (where a crash
#       already happened inside). See the block at the end of app/huecos.toml.
#    2. A probing run of the release build, which finds them empirically in
#       one go.
# ============================================================================

import argparse
import bisect
import json
import os
import re
import sys

RE_FUNC = re.compile(r'^DEFINE_REX_FUNC\((?:sub_)?([0-9A-Fa-f]{8})\)')


def medir_funciones(gen_dir):
    """Returns {start: instruction_count} by walking the generated C++."""
    tamanos = {}
    ficheros = sorted(f for f in os.listdir(gen_dir) if f.endswith('.cpp'))
    for idx, nombre in enumerate(ficheros, 1):
        ruta = os.path.join(gen_dir, nombre)
        actual = None
        n = 0
        with open(ruta, 'r', encoding='utf-8', errors='replace') as fh:
            for linea in fh:
                if linea.startswith('DEFINE_REX_FUNC('):
                    m = RE_FUNC.match(linea)
                    if m:
                        actual = int(m.group(1), 16)
                        n = 0
                elif actual is not None:
                    if linea.startswith('\t// '):
                        n += 1
                    elif linea.startswith('}'):
                        tamanos[actual] = n
                        actual = None
        sys.stdout.write("\r  leidos %d/%d archivos" % (idx, len(ficheros)))
        sys.stdout.flush()
    print()
    return tamanos


def main():
    p = argparse.ArgumentParser(description="Busca huecos de codigo sin funcion.")
    p.add_argument("--gen", default="app/generated/default",
                   help="carpeta con el codigo generado")
    p.add_argument("--min", type=int, default=4,
                   help="tamano minimo de hueco a listar (por defecto 4)")
    p.add_argument("--comprobar", nargs="*", default=[],
                   help="direcciones concretas a localizar, p.ej. 0x8215FEA8")
    p.add_argument("--salida", default="docs/huecos.txt")
    p.add_argument("--toml", default="tools/huecos_functions.toml")
    args = p.parse_args()

    part = os.path.join(args.gen, "codegen.partition.json")
    if not os.path.exists(part):
        raise SystemExit("No existe %s. Lanza antes el codegen." % part)

    print("Leyendo reparto de funciones...")
    asignaciones = json.load(open(part))["assignments"]
    inicios = sorted(int(k, 16) for k in asignaciones)
    print("  %d funciones" % len(inicios))

    print("Midiendo cada funcion en el C++ generado...")
    tamanos = medir_funciones(args.gen)
    print("  %d medidas" % len(tamanos))

    faltan = [a for a in inicios if a not in tamanos]
    if faltan:
        print("  aviso: %d funciones sin medir (se ignoran)" % len(faltan))

    # Calcular huecos
    huecos = []
    for i, a in enumerate(inicios[:-1]):
        n = tamanos.get(a)
        if n is None:
            continue
        fin = a + 4 * n
        siguiente = inicios[i + 1]
        if fin < siguiente:
            huecos.append((fin, siguiente - fin, a, siguiente))

    grandes = [h for h in huecos if h[1] >= args.min]

    # Distribution by size
    dist = {}
    for _, tam, _, _ in huecos:
        dist[tam] = dist.get(tam, 0) + 1

    lineas = []
    def w(s=""):
        print(s)
        lineas.append(s)

    w()
    w("=" * 60)
    w("  HUECOS DE CODIGO SIN FUNCION ASIGNADA")
    w("=" * 60)
    w("Funciones            : %d" % len(inicios))
    w("Huecos totales       : %d" % len(huecos))
    w("Huecos >= %d bytes    : %d" % (args.min, len(grandes)))
    w("Bytes en huecos      : %d" % sum(h[1] for h in huecos))
    w()
    w("Distribucion por tamano de hueco:")
    for tam in sorted(dist):
        w("   %5d bytes  x %d" % (tam, dist[tam]))
    w()

    if args.comprobar:
        w("-" * 60)
        w("Direcciones consultadas:")
        finales = sorted(h[0] for h in huecos)
        for s in args.comprobar:
            t = int(s, 16)
            es_inicio = bisect.bisect_left(inicios, t) < len(inicios) and \
                        inicios[bisect.bisect_left(inicios, t)] == t
            dentro = None
            for fin, tam, ini, sig in huecos:
                if fin <= t < fin + tam:
                    dentro = (fin, tam, ini, sig)
                    break
            w("  0x%08X  inicio de funcion: %s" % (t, "SI" if es_inicio else "NO"))
            if dentro:
                w("      cae en hueco de %d bytes: 0x%08X - 0x%08X"
                  % (dentro[1], dentro[0], dentro[0] + dentro[1]))
                w("      (tras la funcion 0x%08X, antes de 0x%08X)"
                  % (dentro[2], dentro[3]))
            elif not es_inicio:
                w("      no cae en ningun hueco conocido")
        w()

    w("-" * 60)
    w("Primeros 60 huecos de >= %d bytes:" % args.min)
    for fin, tam, ini, sig in grandes[:60]:
        w("  0x%08X  %5d bytes   (tras 0x%08X, antes de 0x%08X)"
          % (fin, tam, ini, sig))
    if len(grandes) > 60:
        w("  ... y %d mas" % (len(grandes) - 60))

    os.makedirs(os.path.dirname(args.salida) or ".", exist_ok=True)
    with open(args.salida, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lineas) + "\n")
        fh.write("\n" + "-" * 60 + "\nTODOS los huecos >= %d bytes:\n" % args.min)
        for fin, tam, ini, sig in grandes:
            fh.write("  0x%08X  %5d bytes   (tras 0x%08X, antes de 0x%08X)\n"
                     % (fin, tam, ini, sig))
    print()
    print("Informe completo en %s" % args.salida)

    os.makedirs(os.path.dirname(args.toml) or ".", exist_ok=True)
    with open(args.toml, "w", encoding="utf-8") as fh:
        fh.write("# Generado por tools/huecos.py - NO incluir tal cual.\n")
        fh.write("# Declarar una funcion en un hueco que sea relleno o datos puede\n")
        fh.write("# romper el codegen. Copiar solo las entradas que hagan falta.\n")
        fh.write("[functions]\n")
        for fin, tam, ini, sig in grandes:
            fh.write('"0x%08X" = { }   # %d bytes, tras 0x%08X\n' % (fin, tam, ini))
    print("Bloque TOML de referencia en %s" % args.toml)


if __name__ == "__main__":
    main()
