#pragma once

// Adapter for the 2005 XDK container. It does not use the shader tag as an
// offset or as a length: the microcode starts at virtualSize.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace nfsmw {
struct Lector {
  const std::vector<uint8_t>& datos;
  void rango(size_t o, size_t n) const {
    if (o > datos.size() || n > datos.size() - o)
      throw std::runtime_error("contenedor truncado o desplazamiento fuera del archivo");
  }
  uint32_t u32(size_t o) const {
    rango(o, 4);
    return uint32_t(datos[o]) << 24 | uint32_t(datos[o+1]) << 16 |
           uint32_t(datos[o+2]) << 8 | datos[o+3];
  }
};

struct Flujo {
  uint32_t bytes = 0;
  uint32_t instrucciones = 0;
};

// The CF limit is deduced from the EXEC addresses, as in Xenia. Each interval
// is also checked before the translator accesses its operands.
inline Flujo ValidarMicrocodigo(const Lector& l, uint32_t inicio, uint32_t longitud) {
  l.rango(inicio, longitud);
  if (!longitud || longitud % 12) throw std::runtime_error("longitud de microcodigo invalida");
  const uint32_t bloques = longitud / 12;
  uint32_t limite = bloques, ejecutadas = 0;
  bool termina = false;
  for (uint32_t i = 0; i < limite; ++i) {
    const uint32_t a = l.u32(inicio + i*12), b = l.u32(inicio + i*12+4), c = l.u32(inicio + i*12+8);
    const uint64_t cf[] = {uint64_t(a) | uint64_t(b & 65535) << 32,
                           uint64_t(b >> 16) | uint64_t(c) << 16};
    for (uint64_t palabra : cf) {
      const uint32_t op = (palabra >> 44) & 15;
      if ((op >= 1 && op <= 6) || op == 13 || op == 14) {
        const uint32_t direccion = palabra & 4095, cantidad = (palabra >> 12) & 7;
        if (!direccion || cantidad > 6 || direccion >= bloques || cantidad > bloques - direccion || direccion <= i)
          throw std::runtime_error("EXEC fuera del microcodigo; posible volcado de memoria contigua incorrecto");
        limite = std::min(limite, direccion);
        ejecutadas += cantidad;
        termina |= op == 2 || op == 4 || op == 6 || op == 14;
      }
    }
  }
  if (!ejecutadas || !termina)
    throw std::runtime_error("sin EXEC util y terminacion; no se acepta un HLSL vacio como traduccion");
  return {limite * 12, ejecutadas};
}

inline std::vector<uint8_t> Convertir2005(const std::vector<uint8_t>& entrada, Flujo& flujo) {
  const Lector l{entrada};
  l.rango(0, 24);
  const uint32_t firma = l.u32(0), vs = l.u32(4), ps = l.u32(8);
  if ((firma & 0xFFFFFFFEu) != 0x102A0E00u || vs < 24 || uint64_t(vs) + ps != entrada.size())
    throw std::runtime_error("cabecera de 2005 invalida");
  const bool pixel = !(firma & 1);
  const uint32_t def = l.u32(12), ct = l.u32(16), sh = l.u32(20);
  const auto virtualRango = [&](size_t o, size_t n) {
    if (o > vs || n > vs - o) throw std::runtime_error("tabla fuera de la parte virtual");
  };
  virtualRango(sh, pixel ? 32 : 40);
  virtualRango(ct, 32);
  // The CTAB table keeps the same format and offsets relative to ct+4.
  const uint32_t baseCt = ct + 4, constantes = l.u32(baseCt + 12);
  virtualRango(size_t(baseCt) + l.u32(baseCt + 16), size_t(constantes)*20);
  for (uint32_t i = 0; i < constantes; ++i) {
    const size_t info = size_t(baseCt) + l.u32(baseCt + 16) + i*20;
    const size_t nombre = size_t(baseCt) + l.u32(info);
    virtualRango(nombre, 1);
    if (!std::memchr(entrada.data() + nombre, 0, vs - nombre)) throw std::runtime_error("nombre sin terminador");
    virtualRango(size_t(baseCt) + l.u32(info + 12), 16);
  }
  flujo = ValidarMicrocodigo(l, vs, ps);

  // The original table is kept and normalized structures are appended. Its
  // internal offsets are relative and do not change when it is shifted by twelve bytes.
  std::vector<uint8_t> s(vs + 12, 0), fisica(entrada.begin() + vs, entrada.end());
  std::copy(entrada.begin()+24, entrada.begin()+vs, s.begin()+36);
  const auto poner = [&](size_t o, uint32_t v) {
    if (o + 4 > s.size()) s.resize(o + 4);
    s[o] = v >> 24; s[o+1] = v >> 16; s[o+2] = v >> 8; s[o+3] = v;
  };
  const auto anexar = [&](uint32_t v) { const size_t o = s.size(); poner(o, v); };
  poner(0, 0x102A1100u | (firma & 1));
  poner(16, ct + 12);
  poner(24, s.size());
  const uint32_t interpoladores = (l.u32(sh + 16) >> 5) & 31;
  anexar(0);                 // physicalOffset: CF starts at the first byte.
  anexar(flujo.bytes);       // Length checked against the EXEC addresses.
  anexar(0);
  anexar(l.u32(sh + 16));
  anexar(0);
  anexar(interpoladores << 5);
  if (pixel) {
    virtualRango(sh + 32, size_t(interpoladores)*4);
    const uint32_t salidas = l.u32(sh + 28);
    if (salidas & ~0x2Fu) throw std::runtime_error("mascara de salidas de 2005 desconocida");
    anexar(0);
    // In this corpus bit 5 matches KILL, not a depth output.
    // The test checks the outputs against the microcode exports.
    anexar(salidas & 15);
    for (uint32_t i = 0; i < interpoladores; ++i) anexar(l.u32(sh + 32 + i*4));
  } else {
    const uint32_t previos = l.u32(sh + 24), elementos = l.u32(sh + 28);
    const size_t comienzo = size_t(sh) + 40 + size_t(previos)*4;
    virtualRango(comienzo, (size_t(elementos) + interpoladores)*4);
    anexar(0); anexar(elementos); anexar(0);
    for (uint32_t i = 0; i < elementos + interpoladores; ++i) anexar(l.u32(comienzo + i*4));
  }

  if (def) {
    virtualRango(def, 24);
    const size_t fin = size_t(def) + 24 + l.u32(def + 16);
    virtualRango(def, fin - def);
    poner(20, s.size());
    for (int i = 0; i < 5; ++i) anexar(0);
    size_t o = def + 24;
    while (o + 4 <= fin && l.u32(o)) {
      const uint32_t palabra = l.u32(o), registro = palabra >> 16, cantidad = palabra & 65535;
      o += 4;
      if (registro < 0x300 || registro % 16 || !cantidad || cantidad % 4 || size_t(cantidad)*4 > fin - o)
        throw std::runtime_error("definicion inmediata de 2005 no admitida");
      // The driver copies to device+0x480+register; c0 starts at +0x780.
      anexar(((registro - 0x300) / 16) << 16 | cantidad);
      anexar(fisica.size());
      fisica.insert(fisica.end(), entrada.begin()+o, entrada.begin()+o+cantidad*4);
      o += cantidad*4;
    }
    if (o + 8 > fin || l.u32(o) || l.u32(o+4))
      throw std::runtime_error("definiciones enmascaradas pendientes de implementar");
    anexar(0); anexar(0); anexar(0);
  }
  poner(4, s.size());
  poner(8, fisica.size());
  s.insert(s.end(), fisica.begin(), fisica.end());
  return s;
}
}  // namespace nfsmw
