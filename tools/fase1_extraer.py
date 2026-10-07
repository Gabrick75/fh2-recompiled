#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NFSMW Recomp - phase 1: extract the Xbox 360 ISO and dump the XEX info.

No dependencies: only the Python 3.8+ standard library.

Typical usage:
    python tools/fase1_extraer.py "D:\\dumps\\NFSMW.iso" --listar
    python tools/fase1_extraer.py "D:\\dumps\\NFSMW.iso" -o assets/game_root
    python tools/fase1_extraer.py assets/game_root/default.xex --info

What it does:
  1. Detects the offset of the game partition (XGD1 / XGD2 / XGD3 / raw).
  2. Walks the XDVDFS tree and lists or extracts the files.
  3. Parses the XEX2 header of default.xex and gets the title id, base address,
     entry point and the compression/encryption type.

It does not decrypt or decompress the PE: ReXGlue takes care of that internally
during codegen. Only the files and the header data are needed here.
"""

import argparse
import os
import struct
import sys

SECTOR = 2048
XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"

# Known offsets where the game partition starts, by disc type.
KNOWN_BASES = [
    (0x00000000, "particion cruda / imagen ya recortada"),
    (0x0FD90000, "XGD2 (la mayoria de juegos de 360)"),
    (0x02080000, "XGD3 (titulos tardios)"),
    (0x18300000, "XGD1 (Xbox original)"),
]

ATTR_DIRECTORY = 0x10


# ---------------------------------------------------------------------------
# XDVDFS
# ---------------------------------------------------------------------------

def _magic_at(fh, offset):
    """True if the volume descriptor is at base=offset."""
    try:
        fh.seek(offset + 32 * SECTOR)
    except OSError:
        return False
    head = fh.read(len(XDVDFS_MAGIC))
    return head == XDVDFS_MAGIC


def detectar_base(fh, limite_scan=1 << 30):
    """Returns (base, description) of the game partition."""
    for base, desc in KNOWN_BASES:
        if _magic_at(fh, base):
            return base, desc

    # None of the known ones: brute-force scan in large chunks.
    fh.seek(0, os.SEEK_END)
    tam = fh.tell()
    tope = min(tam, limite_scan)
    CHUNK = 16 << 20
    solapa = len(XDVDFS_MAGIC)
    pos = 0
    while pos < tope:
        fh.seek(pos)
        buf = fh.read(CHUNK + solapa)
        if not buf:
            break
        idx = buf.find(XDVDFS_MAGIC)
        while idx != -1:
            abs_off = pos + idx
            if abs_off % SECTOR == 0 and abs_off >= 32 * SECTOR:
                base = abs_off - 32 * SECTOR
                if _magic_at(fh, base):
                    return base, "detectado por barrido (offset no estandar)"
            idx = buf.find(XDVDFS_MAGIC, idx + 1)
        pos += CHUNK

    raise SystemExit(
        "No se encontro un sistema de archivos XDVDFS en la imagen.\n"
        "Comprueba que es un ISO de Xbox 360 y no un CCI/GOD/ZAR comprimido."
    )


def leer_descriptor(fh, base):
    """Returns (sector_raiz, tam_raiz) by reading the volume descriptor."""
    fh.seek(base + 32 * SECTOR)
    vd = fh.read(SECTOR)
    if len(vd) < SECTOR or vd[:20] != XDVDFS_MAGIC:
        raise SystemExit("Descriptor de volumen invalido.")
    if vd[0x7EC:0x7EC + 20] != XDVDFS_MAGIC:
        print("  aviso: falta el magic de cierre en 0x7EC (imagen truncada?)",
              file=sys.stderr)
    sector_raiz, tam_raiz = struct.unpack_from("<II", vd, 0x14)
    return sector_raiz, tam_raiz


def _entradas(tabla, offset, vistos):
    """Walks the binary tree of a directory. Yields one dict per entry."""
    pila = [offset]
    while pila:
        off = pila.pop()
        if off in vistos:
            continue
        # An offset of 0 is only valid for the root of the tree.
        if off != 0 and off == 0:
            continue
        if off + 14 > len(tabla):
            continue
        vistos.add(off)

        izq, der, sector, tam, attrs, largo = struct.unpack_from(
            "<HHIIBB", tabla, off)

        # 0xFFFF and 0 mean "no child".
        for hijo in (izq, der):
            if hijo not in (0, 0xFFFF):
                pila.append(hijo * 4)

        fin_nombre = off + 14 + largo
        if largo == 0 or fin_nombre > len(tabla):
            continue
        nombre = tabla[off + 14:fin_nombre].decode("latin-1")

        yield {
            "nombre": nombre,
            "sector": sector,
            "tam": tam,
            "dir": bool(attrs & ATTR_DIRECTORY),
        }


def recorrer(fh, base, sector, tam, prefijo=""):
    """Walks the directory tree recursively. Yields (path, entry)."""
    if tam == 0 or tam > (256 << 20):
        return
    fh.seek(base + sector * SECTOR)
    tabla = fh.read(tam)
    if len(tabla) < tam:
        print("  aviso: tabla de directorio truncada en %s" % (prefijo or "/"),
              file=sys.stderr)

    vistos = set()
    hijos = list(_entradas(tabla, 0, vistos))
    hijos.sort(key=lambda e: e["nombre"].lower())

    for e in hijos:
        ruta = prefijo + "/" + e["nombre"] if prefijo else e["nombre"]
        yield ruta, e
        if e["dir"]:
            for sub in recorrer(fh, base, e["sector"], e["tam"], ruta):
                yield sub


def extraer(fh, base, entrada, destino):
    os.makedirs(os.path.dirname(destino) or ".", exist_ok=True)
    fh.seek(base + entrada["sector"] * SECTOR)
    restante = entrada["tam"]
    with open(destino, "wb") as out:
        while restante > 0:
            trozo = fh.read(min(1 << 20, restante))
            if not trozo:
                raise SystemExit(
                    "Fin de archivo inesperado leyendo %s. Imagen incompleta?"
                    % entrada["nombre"])
            out.write(trozo)
            restante -= len(trozo)


# ---------------------------------------------------------------------------
# XEX2
# ---------------------------------------------------------------------------

CLAVES_XEX = {
    0x000002FF: "Resource info",
    0x000003FF: "File format info",
    0x000005FF: "Delta patch descriptor",
    0x000080FF: "Bounding path",
    0x00008105: "Device ID",
    0x00010001: "Original base address",
    0x00010100: "Entry point",
    0x00010201: "Image base address",
    0x000103FF: "Import libraries",
    0x00018002: "Checksum / timestamp",
    0x00018102: "Enabled for callcap",
    0x00018200: "Enabled for fastcap",
    0x000183FF: "Original PE name",
    0x000200FF: "Static libraries",
    0x00020104: "TLS info",
    0x00020200: "Default stack size",
    0x00020301: "Default filesystem cache size",
    0x00020401: "Default heap size",
    0x00028002: "Page heap size and flags",
    0x00030000: "System flags",
    0x00040006: "Execution info",
    0x00040201: "Title workspace size",
    0x00040310: "Game ratings",
    0x00040404: "LAN key",
    0x000405FF: "Xbox 360 logo",
    0x000406FF: "Multidisc media IDs",
    0x000407FF: "Alternate title IDs",
    0x00040801: "Additional title memory",
    0x00E10402: "Exports by name",
}

COMPRESION = {0: "ninguna", 1: "basica", 2: "normal (LZX)", 3: "delta"}
CIFRADO = {0: "ninguno", 1: "normal (AES-128)"}


def _u32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def info_xex(ruta):
    with open(ruta, "rb") as fh:
        cab = fh.read(0x1000)
        if cab[:4] != b"XEX2":
            raise SystemExit(
                "%s no empieza con el magic 'XEX2'. No es un ejecutable de "
                "Xbox 360 (o esta cifrado con otro formato)." % ruta)

        flags_modulo = _u32(cab, 0x04)
        off_pe = _u32(cab, 0x08)
        off_seguridad = _u32(cab, 0x10)
        n_opt = _u32(cab, 0x14)

        print("== Cabecera XEX2 ==")
        print("  archivo               : %s (%s bytes)"
              % (ruta, f"{os.path.getsize(ruta):,}"))
        print("  module flags          : 0x%08X" % flags_modulo)
        print("  offset datos PE       : 0x%08X" % off_pe)
        print("  offset security info  : 0x%08X" % off_seguridad)
        print("  cabeceras opcionales  : %d" % n_opt)

        # The optional headers can extend past the 0x1000 bytes read.
        fh.seek(0x18)
        raw_opt = fh.read(n_opt * 8)
        opcionales = {}
        for i in range(n_opt):
            clave, valor = struct.unpack_from(">II", raw_opt, i * 8)
            opcionales[clave] = valor

        entry = opcionales.get(0x00010100)
        base_img = opcionales.get(0x00010201)

        print()
        print("== Datos clave ==")

        # Execution info -> title id, version, disco
        title_id = None
        if 0x00040006 in opcionales:
            fh.seek(opcionales[0x00040006])
            ei = fh.read(24)
            if len(ei) == 24:
                media_id, version, base_version, title_id = struct.unpack_from(
                    ">IIII", ei, 0)
                plataforma, tabla_exe, disco_n, disco_tot = struct.unpack_from(
                    ">BBBB", ei, 0x10)
                print("  Title ID              : %08X" % title_id)
                print("  Media ID              : %08X" % media_id)
                print("  Version               : %d.%d.%d.%d"
                      % ((version >> 28) & 0xF, (version >> 16) & 0xFFF,
                         (version >> 8) & 0xFF, version & 0xFF))
                print("  Disco                 : %d de %d" % (disco_n, disco_tot))
        else:
            print("  Title ID              : (sin execution info)")

        if base_img is not None:
            print("  Image base address    : 0x%08X" % base_img)
        if entry is not None:
            print("  Entry point           : 0x%08X" % entry)

        # Security info -> load address, image size
        fh.seek(off_seguridad)
        si = fh.read(0x184)
        if len(si) >= 0x114:
            tam_imagen = _u32(si, 0x004)
            load_addr = _u32(si, 0x110)
            print("  Load address          : 0x%08X" % load_addr)
            print("  Tamano de imagen      : %s bytes" % f"{tam_imagen:,}")

        # File format info -> compresion / cifrado
        if 0x000003FF in opcionales:
            fh.seek(opcionales[0x000003FF])
            ffi = fh.read(8)
            if len(ffi) == 8:
                _tam, cif, comp = struct.unpack(">IHH", ffi)
                print("  Cifrado               : %s (%d)"
                      % (CIFRADO.get(cif, "desconocido"), cif))
                print("  Compresion            : %s (%d)"
                      % (COMPRESION.get(comp, "desconocida"), comp))

        # Other useful fields stored inline
        if 0x00010001 in opcionales:
            print("  Original base address : 0x%08X" % opcionales[0x00010001])
        if 0x00020200 in opcionales:
            print("  Default stack size    : %s bytes" % f"{opcionales[0x00020200]:,}")
        if 0x00030000 in opcionales:
            print("  System flags          : 0x%08X" % opcionales[0x00030000])

        # Import libraries: which kernel modules the game uses.
        # Each module brings imports that the runtime has to implement or stub.
        if 0x000103FF in opcionales:
            try:
                fh.seek(opcionales[0x000103FF])
                cab_imp = fh.read(12)
                if len(cab_imp) == 12:
                    _total, st_tam, st_num = struct.unpack(">III", cab_imp)
                    if 0 < st_tam <= (1 << 16):
                        tabla = fh.read(st_tam)
                        nombres = [n.decode("latin-1") for n in tabla.split(b"\x00")
                                   if n and all(32 <= c < 127 for c in n)]
                        if nombres:
                            print()
                            print("== Modulos importados (%d) ==" % st_num)
                            for n in nombres:
                                print("  " + n)
            except (OSError, struct.error):
                pass

        print()
        print("== Cabeceras opcionales presentes ==")
        for clave in sorted(opcionales):
            nombre = CLAVES_XEX.get(clave, "")
            print("  0x%08X  %-24s valor/offset 0x%08X"
                  % (clave, nombre, opcionales[clave]))

        print()
        if title_id == 0x454107D9:
            print("  >> Title ID coincide con Need for Speed: Most Wanted (2005). Correcto.")
        elif title_id is not None:
            print("  >> OJO: el Title ID esperado para NFSMW 2005 es 454107D9.")
            print("     Este XEX es 0x%08X. Comprueba que dumpeaste el juego correcto." % title_id)

        print()
        print("Copia esta salida a docs/xex_info.txt: vas a necesitar el base")
        print("address cada vez que declares una direccion en el TOML.")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def buscar_iso():
    """Looks for a single .iso in the project folder (or in the current one)."""
    raiz = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    candidatos = []
    for carpeta in (raiz, os.getcwd()):
        try:
            for n in os.listdir(carpeta):
                if n.lower().endswith(".iso"):
                    ruta = os.path.join(carpeta, n)
                    if ruta not in candidatos:
                        candidatos.append(ruta)
        except OSError:
            pass

    if not candidatos:
        raise SystemExit(
            "No se indico ningun archivo y no hay ningun .iso en:\n"
            "  %s\n"
            "Pasa la ruta como argumento:\n"
            "  python tools/fase1_extraer.py \"D:/ruta/al/juego.iso\" --listar" % raiz)

    if len(candidatos) > 1:
        msg = "Hay varios .iso; indica cual quieres:\n"
        for c in candidatos:
            msg += "  %s\n" % c
        raise SystemExit(msg)

    return candidatos[0]


def main():
    p = argparse.ArgumentParser(
        description="Extrae un ISO de Xbox 360 y vuelca la info del XEX.")
    p.add_argument("entrada", nargs="?", default=None,
                   help="ruta al .iso, o a un .xex si usas --info. Si se omite, busca un unico .iso en la carpeta del proyecto.")
    p.add_argument("-o", "--salida", default="assets/game_root",
                   help="carpeta destino de la extraccion (por defecto: assets/game_root)")
    p.add_argument("--listar", action="store_true",
                   help="solo listar el contenido, sin extraer nada")
    p.add_argument("--solo-xex", action="store_true",
                   help="extraer unicamente default.xex (y el .xexp si existe)")
    p.add_argument("--info", action="store_true",
                   help="la entrada es un .xex: volcar su cabecera y salir")
    args = p.parse_args()

    if args.entrada is None:
        args.entrada = buscar_iso()
        print("ISO encontrado automaticamente: %s\n" % args.entrada)

    if not os.path.exists(args.entrada):
        raise SystemExit("No existe el archivo: %s" % args.entrada)

    if args.info or args.entrada.lower().endswith(".xex"):
        info_xex(args.entrada)
        return

    with open(args.entrada, "rb") as fh:
        base, desc = detectar_base(fh)
        print("Particion de juego en offset 0x%08X  (%s)" % (base, desc))

        sector_raiz, tam_raiz = leer_descriptor(fh, base)
        print("Directorio raiz: sector %d, %s bytes\n" % (sector_raiz, f"{tam_raiz:,}"))

        entradas = list(recorrer(fh, base, sector_raiz, tam_raiz))
        if not entradas:
            raise SystemExit("El sistema de archivos esta vacio. Imagen corrupta?")

        n_arch = sum(1 for _, e in entradas if not e["dir"])
        total = sum(e["tam"] for _, e in entradas if not e["dir"])
        print("%d archivos, %d directorios, %s bytes en total\n"
              % (n_arch, len(entradas) - n_arch, f"{total:,}"))

        if args.listar:
            for ruta, e in entradas:
                if e["dir"]:
                    print("  [dir]  %s" % ruta)
                else:
                    print("  %12s  %s" % (f"{e['tam']:,}", ruta))
            return

        objetivos = entradas
        if args.solo_xex:
            objetivos = [(r, e) for r, e in entradas
                         if not e["dir"] and
                         (r.lower().endswith(".xex") or r.lower().endswith(".xexp"))]
            if not objetivos:
                raise SystemExit("No hay ningun .xex en la imagen.")

        hechos = 0
        for ruta, e in objetivos:
            if e["dir"]:
                continue
            destino = os.path.join(args.salida, ruta.replace("/", os.sep))
            extraer(fh, base, e, destino)
            hechos += 1
            if hechos % 50 == 0:
                print("  ... %d archivos" % hechos)
        print("\n%d archivos extraidos en %s" % (hechos, args.salida))

    xex = os.path.join(args.salida, "default.xex")
    if os.path.exists(xex):
        print()
        info_xex(xex)
    else:
        print("\nAviso: no aparecio un default.xex en la raiz. Revisa el listado "
              "con --listar para ver donde esta el ejecutable.")


if __name__ == "__main__":
    main()
