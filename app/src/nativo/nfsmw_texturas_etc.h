// BC1-5 to ETC2/EAC on the CPU, for GPUs without BC (Mali). Each target block is the size of its source block:
//   BC1 (8 bytes)    -> ETC2 RGB8A1 (8)      punch-through alpha keeps BC1's transparent black
//   BC2, BC3 (16)    -> ETC2 RGBA8 (16)      EAC alpha, then ETC2 colour
//   BC4 (8)          -> EAC R11 (8)
//   BC5 (16)         -> EAC RG11 (16)
// So the mip and layer layout, the level offsets and the upload regions stay those of the BC texture: only the
// bytes change, and the image keeps the memory and bandwidth of a compressed one instead of 4 to 8 times that in
// RGBA8 (nfsmw_texturas_bc.h). Each block is decoded with bc::Bloque and encoded again; the encoder is a quick
// one (one base colour per subblock, a fitted plane, two clusters for T and H), not a best-quality search.
// GPU-independent like nfsmw_texturas_bc.h, so it can be checked on PC against another decoder
// (tools/probar_etc.py).
#pragma once

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

#include "nfsmw_texturas_bc.h"

namespace nfsmw::etc {
// EAC modifier tables (Khronos Data Format, table 70). In each row 3, 2, 1, 0, 4, 5, 6, 7 go up.
inline constexpr int8_t kEac[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14},   {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12},   {-3, -6, -8, -12, 2, 5, 7, 11},  {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10},   {-3, -5, -8, -11, 2, 4, 7, 10},  {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},    {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},    {-1, -2, -3, -10, 0, 1, 2, 9},   {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8}};
inline constexpr uint8_t kEacOrden[8] = {3, 2, 1, 0, 4, 5, 6, 7};
// ETC1 intensity tables: small and large modifier.
inline constexpr int kEtc1[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
// T and H distances.
inline constexpr int kDistancia[8] = {3, 6, 11, 16, 23, 32, 41, 64};

inline int Saturar(int v, int maximo) { return v < 0 ? 0 : v > maximo ? maximo : v; }
inline int Expandir4(int v) { return v * 17; }
inline int Expandir5(int v) { return (v << 3) | (v >> 2); }
inline int Expandir6(int v) { return (v << 2) | (v >> 4); }
inline int Expandir7(int v) { return (v << 1) | (v >> 6); }
inline int Cuantizar(int v, int maximo) { return Saturar((v * maximo + 127) / 255, maximo); }

inline void Escribir64(uint64_t v, uint8_t* p) {
  for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (56 - 8 * i));
}

// ---- EAC --------------------------------------------------------------------------------------------------------

// valores: the 16 texels in ETC order (x * 4 + y), 0-255. r11: an EAC R11 block (8 * base + 4 + modifier *
// multiplier * 8 over 2047; multiplier 0 adds the modifier alone), else the alpha of ETC2 RGBA8 (base + modifier *
// multiplier over 255; multiplier 0 is not allowed). rapido: two tables instead of sixteen.
inline void Eac(const uint8_t* valores, bool r11, uint8_t* salida, bool rapido = false) {
  int objetivo[16];
  int bajo = INT_MAX, alto = INT_MIN;
  for (int i = 0; i < 16; ++i) {
    objetivo[i] = r11 ? (valores[i] * 2047 + 127) / 255 : valores[i];
    bajo = std::min(bajo, objetivo[i]);
    alto = std::max(alto, objetivo[i]);
  }
  const int maximo = r11 ? 2047 : 255;
  uint64_t mejor_bits = 0;
  int64_t mejor_error = INT64_MAX;
  const auto probar = [&](int tabla, int base, int mult) {
    int decodificado[8];
    for (int k = 0; k < 8; ++k) {
      const int m = kEac[tabla][kEacOrden[k]];
      decodificado[k] = r11 ? Saturar(base * 8 + 4 + (mult ? m * mult * 8 : m), maximo)
                            : Saturar(base + m * mult, maximo);
    }
    int64_t error = 0;
    uint64_t indices = 0;
    for (int i = 0; i < 16; ++i) {
      // decodificado goes up (saturated, never down): walk until the error grows.
      int k = 0, d = decodificado[0] - objetivo[i], e = d * d;
      while (k < 7) {
        const int d2 = decodificado[k + 1] - objetivo[i];
        if (d2 * d2 > e) break;
        e = d2 * d2;
        ++k;
      }
      error += e;
      if (error >= mejor_error) return;
      indices = (indices << 3) | kEacOrden[k];
    }
    mejor_error = error;
    mejor_bits = (uint64_t(base) << 56) | (uint64_t(mult) << 52) | (uint64_t(tabla) << 48) | indices;
  };
  // In base units: the value itself for alpha, (value - 4) / 8 for R11.
  const double u_bajo = r11 ? (bajo - 4) / 8.0 : bajo, u_alto = r11 ? (alto - 4) / 8.0 : alto;
  if (bajo == alto) {  // flat: table 13 has a 0 (alpha), and -3 to 2 for the R11 remainder
    probar(13, r11 ? bajo / 8 : bajo, r11 ? 0 : 1);  // bajo / 8: (bajo - 4) / 8, rounded
  }
  // 15 has its eight levels about evenly spaced and 11 a little wider at the bottom.
  static constexpr int kTodas[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  static constexpr int kRapidas[2] = {15, 11};
  for (int j = 0; j < (rapido ? 2 : 16) && mejor_error; ++j) {
    const int t = rapido ? kRapidas[j] : kTodas[j];
    const int mn = kEac[t][3], mx = kEac[t][7];
    const int mult = std::clamp(int(std::lround((u_alto - u_bajo) / (mx - mn))), 1, 15);
    probar(t, Saturar(int(std::lround((u_bajo + u_alto) / 2 - mult * (mn + mx) / 2.0)), 255), mult);
    if (r11 && alto - bajo <= 2 * (mx - mn)) {
      probar(t, Saturar(int(std::lround(((bajo + alto) / 2.0 - 4 - (mn + mx) / 2.0) / 8)), 255), 0);
    }
  }
  Escribir64(mejor_bits, salida);
}

// ---- ETC2 colour ------------------------------------------------------------------------------------------------

inline int Error3(const int* a, const uint8_t* b) {
  const int r = a[0] - b[0], g = a[1] - b[1], bl = a[2] - b[2];
  return r * r + g * g + bl * bl;
}

// Apart from planar, every ETC2 colour is a base plus the same m on R, G and B. Against texel p that costs
// |base - p|^2 + 2m * sum(base - p) + 3m^2 while no channel saturates, so the two sums are taken once per base and
// each modifier is one multiply-add instead of three saturations and a distance.
struct Desplazado {
  int base[3], minimo, maximo, d[16], s[16];
  void Preparar(const int* b, const uint8_t (*pixeles)[3]) {
    std::copy_n(b, 3, base);
    minimo = std::min({b[0], b[1], b[2]});
    maximo = std::max({b[0], b[1], b[2]});
    for (int i = 0; i < 16; ++i) {
      const int r = b[0] - pixeles[i][0], g = b[1] - pixeles[i][1], bl = b[2] - pixeles[i][2];
      d[i] = r * r + g * g + bl * bl;
      s[i] = r + g + bl;
    }
  }
  int Error(int i, int m, const uint8_t* p) const {
    if (minimo + m >= 0 && maximo + m <= 255) return d[i] + 2 * m * s[i] + 3 * m * m;
    const int color[3] = {Saturar(base[0] + m, 255), Saturar(base[1] + m, 255), Saturar(base[2] + m, 255)};
    return Error3(color, p);
  }
  // The best of n offsets (in ascending order) for texel i: the error is a parabola with its lowest point at
  // -s / 3, so the nearest offset, give or take saturation. Returns its position and its exact error.
  int Mejor(int i, const int* desplazamientos, int n, const uint8_t* p, int& posicion) const {
    int j = 0;
    while (j + 1 < n && 3 * (desplazamientos[j] + desplazamientos[j + 1]) < -2 * s[i]) ++j;
    posicion = j;
    return Error(i, desplazamientos[j], p);
  }
};

// Sets the free bits listed in `libres` so that the R, G and B pairs (5-bit base + signed 3-bit difference)
// overflow exactly where `desborda` says (bit 0 R, 1 G, 2 B). The overflow is what tells T, H and planar apart.
inline uint64_t AjustarDesbordes(uint64_t bits, std::span<const int> libres, uint32_t desborda) {
  for (uint32_t combinacion = 0; combinacion < (1u << libres.size()); ++combinacion) {
    uint64_t prueba = bits;
    for (size_t i = 0; i < libres.size(); ++i) {
      prueba = (prueba & ~(uint64_t(1) << libres[i])) | (uint64_t((combinacion >> i) & 1) << libres[i]);
    }
    uint32_t visto = 0;
    for (int canal = 0; canal < 3; ++canal) {
      const int desplazamiento = 59 - 8 * canal;
      const int base = int((prueba >> desplazamiento) & 31);
      int diferencia = int((prueba >> (desplazamiento - 3)) & 7);
      if (diferencia >= 4) diferencia -= 8;
      if (base + diferencia < 0 || base + diferencia > 31) visto |= 1u << canal;
    }
    // Only the first channel that overflows counts: R decides T, then G decides H, then B decides planar.
    const uint32_t primero = visto & (~visto + 1);
    if (primero == desborda) return prueba;
  }
  return bits;  // not reached: every mode has enough free bits
}

// pixeles: 16 RGB texels in ETC order (x * 4 + y). transparentes: bit i set when texel i is transparent (only with
// a1). a1: an ETC2 RGB8A1 block, which has no individual mode and whose bit 33 says "opaque"; with any texel
// transparent the block is not opaque, index 2 is transparent and, in the differential mode, index 0 is the base
// colour unchanged.
inline void Etc2(const uint8_t (*pixeles)[3], uint16_t transparentes, bool a1, uint8_t* salida) {
  const bool hay_transparentes = a1 && transparentes;
  uint64_t mejor_bits = 0;
  int64_t mejor_error = INT64_MAX;
  const auto transparente = [&](int i) { return hay_transparentes && ((transparentes >> i) & 1); };

  // Individual and differential: two subblocks (2x4, or 4x2 with flip), each a base colour plus one of four
  // intensities from one of eight tables.
  for (int flip = 0; flip < 2 && mejor_error; ++flip) {
    int suma[2][3] = {}, cuenta[2] = {};
    const auto sub = [&](int i) { return flip ? (i & 3) >= 2 : (i >> 2) >= 2; };
    int texeles[2][8], n_texeles[2] = {};
    for (int i = 0; i < 16; ++i) texeles[sub(i)][n_texeles[sub(i)]++] = i;
    for (int i = 0; i < 16; ++i) {
      if (transparente(i)) continue;
      const int s = sub(i);
      for (int c = 0; c < 3; ++c) suma[s][c] += pixeles[i][c];
      ++cuenta[s];
    }
    int media[2][3];
    for (int s = 0; s < 2; ++s) {
      const int fuente = cuenta[s] ? s : cuenta[1 - s] ? 1 - s : -1;
      for (int c = 0; c < 3; ++c) {
        media[s][c] = fuente < 0 ? 0 : (suma[fuente][c] + cuenta[fuente] / 2) / cuenta[fuente];
      }
    }
    for (int diferencial = 1; diferencial >= (a1 ? 1 : 0); --diferencial) {
      int q[2][3], base[2][3];
      for (int c = 0; c < 3; ++c) {
        if (diferencial) {
          q[0][c] = Cuantizar(media[0][c], 31);
          q[1][c] = q[0][c] + std::clamp(Cuantizar(media[1][c], 31) - q[0][c], -4, 3);
          base[0][c] = Expandir5(q[0][c]);
          base[1][c] = Expandir5(q[1][c]);
        } else {
          for (int s = 0; s < 2; ++s) {
            q[s][c] = Cuantizar(media[s][c], 15);
            base[s][c] = Expandir4(q[s][c]);
          }
        }
      }
      int64_t error = 0;
      uint32_t tablas[2] = {}, msb = 0, lsb = 0;
      for (int s = 0; s < 2; ++s) {
        Desplazado desplazado;
        desplazado.Preparar(base[s], pixeles);
        int64_t mejor_sub = INT64_MAX;
        uint32_t msb_sub = 0, lsb_sub = 0;
        for (int t = 0; t < 8; ++t) {
          // Index (msb, lsb): 0 +a, 1 +b, 2 -a, 3 -b. Not opaque: 0 is +0 and 2 transparent.
          const int a = kEtc1[t][0], b = kEtc1[t][1];
          const int opacos[4] = {-b, -a, a, b}, indices_opacos[4] = {3, 2, 0, 1};
          const int sin_opacar[3] = {-b, 0, b}, indices_sin_opacar[3] = {3, 0, 1};
          const int* desplazamientos = hay_transparentes ? sin_opacar : opacos;
          const int* indices = hay_transparentes ? indices_sin_opacar : indices_opacos;
          const int n = hay_transparentes ? 3 : 4;
          int64_t e_tabla = 0;
          uint32_t msb_t = 0, lsb_t = 0;
          for (int j = 0; j < 8 && e_tabla < mejor_sub; ++j) {
            const int i = texeles[s][j];
            int indice = 2, e = 0;
            if (!transparente(i)) {
              int posicion;
              e = desplazado.Mejor(i, desplazamientos, n, pixeles[i], posicion);
              indice = indices[posicion];
            }
            e_tabla += e;
            msb_t |= uint32_t(indice >> 1) << i;
            lsb_t |= uint32_t(indice & 1) << i;
          }
          if (e_tabla < mejor_sub) {
            mejor_sub = e_tabla;
            tablas[s] = t;
            msb_sub = msb_t;
            lsb_sub = lsb_t;
          }
        }
        error += mejor_sub;
        msb |= msb_sub;
        lsb |= lsb_sub;
      }
      if (error >= mejor_error) continue;
      mejor_error = error;
      uint64_t bits = (uint64_t(tablas[0]) << 37) | (uint64_t(tablas[1]) << 34) | (uint64_t(flip) << 32) |
                      (uint64_t(msb) << 16) | lsb;
      // Bit 33: differential (RGB8) or opaque (RGB8A1, always differential).
      bits |= uint64_t(a1 ? !hay_transparentes : diferencial) << 33;
      for (int c = 0; c < 3; ++c) {
        bits |= diferencial ? (uint64_t(q[0][c]) << (59 - 8 * c)) | (uint64_t((q[1][c] - q[0][c]) & 7) << (56 - 8 * c))
                            : (uint64_t(q[0][c]) << (60 - 8 * c)) | (uint64_t(q[1][c]) << (56 - 8 * c));
      }
      mejor_bits = bits;
    }
  }

  // Planar (always opaque): a plane through the colours, for smooth gradients the subblocks show as steps.
  if (!hay_transparentes && mejor_error) {
    int o[3], h[3], v[3], qo[3], qh[3], qv[3];
    for (int c = 0; c < 3; ++c) {
      double media = 0, sx = 0, sy = 0;
      for (int i = 0; i < 16; ++i) {
        media += pixeles[i][c];
        sx += ((i >> 2) - 1.5) * pixeles[i][c];
        sy += ((i & 3) - 1.5) * pixeles[i][c];
      }
      media /= 16;
      const double px = sx / 20, py = sy / 20;  // slope per texel
      const double origen = media - 1.5 * px - 1.5 * py;
      const int maximo = c == 1 ? 127 : 63;
      const auto q = [&](double valor) { return Saturar(int(std::lround(valor * maximo / 255.0)), maximo); };
      qo[c] = q(origen);
      qh[c] = q(origen + 4 * px);
      qv[c] = q(origen + 4 * py);
      o[c] = c == 1 ? Expandir7(qo[c]) : Expandir6(qo[c]);
      h[c] = c == 1 ? Expandir7(qh[c]) : Expandir6(qh[c]);
      v[c] = c == 1 ? Expandir7(qv[c]) : Expandir6(qv[c]);
    }
    int64_t error = 0;
    for (int i = 0; i < 16; ++i) {
      const int x = i >> 2, y = i & 3;
      int color[3];
      for (int c = 0; c < 3; ++c) color[c] = Saturar((x * (h[c] - o[c]) + y * (v[c] - o[c]) + 4 * o[c] + 2) >> 2, 255);
      error += Error3(color, pixeles[i]);
    }
    if (error < mejor_error) {
      mejor_error = error;
      uint64_t bits = (uint64_t(qo[0]) << 57) | (uint64_t(qo[1] >> 6) << 56) | (uint64_t(qo[1] & 63) << 49) |
                      (uint64_t(qo[2] >> 5) << 48) | (uint64_t((qo[2] >> 3) & 3) << 43) |
                      (uint64_t(qo[2] & 7) << 39) | (uint64_t(qh[0] >> 1) << 34) | (uint64_t(1) << 33) |
                      (uint64_t(qh[0] & 1) << 32) | (uint64_t(qh[1]) << 25) | (uint64_t(qh[2]) << 19) |
                      (uint64_t(qv[0]) << 13) | (uint64_t(qv[1]) << 6) | uint64_t(qv[2]);
      static constexpr int kLibres[] = {63, 55, 47, 46, 45, 42};
      mejor_bits = AjustarDesbordes(bits, kLibres, 4);
    }
  }

  // T and H: two colours apart (text, edges, BC1 blocks between two hues), which the subblock modes can only shift
  // in brightness. Only tried when those left a visible error.
  if (mejor_error > 16 * 12) {
    // Two clusters: split along the line between the two texels farthest apart (roughly).
    int lejos_a = -1, lejos_b = -1, maxima = -1;
    for (int i = 0; i < 16; ++i) {
      if (transparente(i)) continue;
      if (lejos_a < 0) lejos_a = i;
      const int centro[3] = {pixeles[lejos_a][0], pixeles[lejos_a][1], pixeles[lejos_a][2]};
      const int e = Error3(centro, pixeles[i]);
      if (e > maxima) { maxima = e; lejos_b = i; }
    }
    maxima = -1;
    for (int i = 0; i < 16 && lejos_b >= 0; ++i) {
      if (transparente(i)) continue;
      const int centro[3] = {pixeles[lejos_b][0], pixeles[lejos_b][1], pixeles[lejos_b][2]};
      const int e = Error3(centro, pixeles[i]);
      if (e > maxima) { maxima = e; lejos_a = i; }
    }
    int suma[2][3] = {}, cuenta[2] = {};
    uint8_t grupo[16] = {};
    for (int i = 0; i < 16 && lejos_b >= 0; ++i) {
      if (transparente(i)) continue;
      const int ca[3] = {pixeles[lejos_a][0], pixeles[lejos_a][1], pixeles[lejos_a][2]};
      const int cb[3] = {pixeles[lejos_b][0], pixeles[lejos_b][1], pixeles[lejos_b][2]};
      const int g = Error3(ca, pixeles[i]) > Error3(cb, pixeles[i]);
      grupo[i] = uint8_t(g);
      for (int c = 0; c < 3; ++c) suma[g][c] += pixeles[i][c];
      ++cuenta[g];
    }
    if (cuenta[0] && cuenta[1]) {
      int q[2][3], base[2][3];
      for (int g = 0; g < 2; ++g) {
        for (int c = 0; c < 3; ++c) {
          q[g][c] = Cuantizar((suma[g][c] + cuenta[g] / 2) / cuenta[g], 15);
          base[g][c] = Expandir4(q[g][c]);
        }
      }
      Desplazado desplazado[2];
      desplazado[0].Preparar(base[0], pixeles);
      desplazado[1].Preparar(base[1], pixeles);
      // Paints: the colours a texel picks from, as two groups of offsets (ascending) from one cluster colour
      // each, with their indices. Returns the error and the indices.
      struct Grupo {
        int colores, n, desplazamientos[3], indices[3];
      };
      const auto pintar = [&](const Grupo& g0, const Grupo& g1, int64_t& error, uint32_t& msb, uint32_t& lsb) {
        error = 0;
        msb = lsb = 0;
        for (int i = 0; i < 16; ++i) {
          int indice = 2, e = 0;
          if (!transparente(i)) {
            int p0, p1;
            const int e0 = desplazado[g0.colores].Mejor(i, g0.desplazamientos, g0.n, pixeles[i], p0);
            const int e1 = desplazado[g1.colores].Mejor(i, g1.desplazamientos, g1.n, pixeles[i], p1);
            e = e0 <= e1 ? e0 : e1;
            indice = e0 <= e1 ? g0.indices[p0] : g1.indices[p1];
          }
          error += e;
          if (error >= mejor_error) return false;
          msb |= uint32_t(indice >> 1) << i;
          lsb |= uint32_t(indice & 1) << i;
        }
        return true;
      };
      // The distances worth trying: a texel's best offset from its cluster colour is about |s| / 3, so the table
      // entries nearest each cluster's mean and largest offset (T wants the spread of one cluster, H of both).
      int media[3] = {}, mayor[2] = {};
      for (int i = 0; i < 16; ++i) {
        if (transparente(i)) continue;
        const int desvio = std::abs(desplazado[grupo[i]].s[i]);
        media[grupo[i]] += desvio;
        media[2] += desvio;
        mayor[grupo[i]] = std::max(mayor[grupo[i]], desvio);
      }
      const int estimadas[5] = {media[0] / (3 * cuenta[0]), media[1] / (3 * cuenta[1]),
                                media[2] / (3 * (cuenta[0] + cuenta[1])), mayor[0] / 3, mayor[1] / 3};
      uint32_t distancias = 0;
      for (const int estimada : estimadas) {
        int k = 0;
        while (k < 7 && 2 * estimada > kDistancia[k] + kDistancia[k + 1]) ++k;
        distancias |= 1u << k;
      }
      const uint64_t opaco = uint64_t(a1 ? !hay_transparentes : 1) << 33;
      for (int d = 0; d < 8; ++d) {
        if (!((distancias >> d) & 1)) continue;
        const int dist = kDistancia[d];
        // T: c1 alone; c2, c2 + d and c2 - d. Either cluster can be the single colour.
        for (int solo = 0; solo < 2; ++solo) {
          const Grupo g0{solo, 1, {0}, {0}};
          const Grupo g1 = hay_transparentes ? Grupo{1 - solo, 2, {-dist, dist}, {3, 1}}
                                             : Grupo{1 - solo, 3, {-dist, 0, dist}, {3, 2, 1}};
          int64_t error;
          uint32_t msb, lsb;
          if (!pintar(g0, g1, error, msb, lsb)) continue;
          mejor_error = error;
          const int* q1 = q[solo];
          const int* q2 = q[1 - solo];
          const uint64_t bits = (uint64_t(q1[0] >> 2) << 59) | (uint64_t(q1[0] & 3) << 56) |
                                (uint64_t(q1[1]) << 52) | (uint64_t(q1[2]) << 48) | (uint64_t(q2[0]) << 44) |
                                (uint64_t(q2[1]) << 40) | (uint64_t(q2[2]) << 36) | (uint64_t(d >> 1) << 34) |
                                opaco | (uint64_t(d & 1) << 32) | (uint64_t(msb) << 16) | lsb;
          static constexpr int kLibres[] = {63, 62, 61, 58};
          mejor_bits = AjustarDesbordes(bits, kLibres, 1);
        }
        // H: c1 + d, c1 - d, c2 + d, c2 - d. The order of the two colours is the distance's lowest bit.
        const int v0 = (q[0][0] << 8) | (q[0][1] << 4) | q[0][2], v1 = (q[1][0] << 8) | (q[1][1] << 4) | q[1][2];
        if (v0 == v1 && !(d & 1)) continue;  // equal colours read as "first >= second": odd distances only
        const int primero = v0 == v1 ? 0 : ((v0 >= v1) == bool(d & 1)) ? 0 : 1;
        const Grupo g0{primero, 2, {-dist, dist}, {1, 0}};
        const Grupo g1 = hay_transparentes ? Grupo{1 - primero, 1, {-dist}, {3}}
                                           : Grupo{1 - primero, 2, {-dist, dist}, {3, 2}};
        int64_t error;
        uint32_t msb, lsb;
        if (!pintar(g0, g1, error, msb, lsb)) continue;
        mejor_error = error;
        const int* q1 = q[primero];
        const int* q2 = q[1 - primero];
        const uint64_t bits = (uint64_t(q1[0]) << 59) | (uint64_t(q1[1] >> 1) << 56) | (uint64_t(q1[1] & 1) << 52) |
                              (uint64_t(q1[2] >> 3) << 51) | (uint64_t(q1[2] & 7) << 47) | (uint64_t(q2[0]) << 43) |
                              (uint64_t(q2[1]) << 39) | (uint64_t(q2[2]) << 35) | (uint64_t(d >> 2) << 34) | opaco |
                              (uint64_t((d >> 1) & 1) << 32) | (uint64_t(msb) << 16) | lsb;
        static constexpr int kLibres[] = {63, 55, 54, 53, 50};
        mejor_bits = AjustarDesbordes(bits, kLibres, 2);
      }
    }
  }
  Escribir64(mejor_bits, salida);
}

// ---- Drafts -----------------------------------------------------------------------------------------------------

// A BC block is a small palette (4 colours, or 8 levels) and an index per texel. The draft encodes the palette,
// not the texels, and sends each texel's BC index through it: about as quick as decoding to RGBA8, for the upload
// that cannot wait. Then the full encode (Etc2, Eac) replaces it from the background.

// The colour half of BC1-3 (bc1: BC1, with its three-colour mode where index 3 is transparent black) to an ETC2
// differential block with the 2x4 split (a1: for ETC2 RGB8A1).
inline void BorradorColor(const uint8_t* color, bool bc1, bool a1, uint8_t* salida) {
  const uint16_t extremos[2] = {bc::Leer16(color), bc::Leer16(color + 2)};
  int paleta[4][3];
  for (int k = 0; k < 2; ++k) {
    const uint32_t r = extremos[k] >> 11, g = (extremos[k] >> 5) & 63, b = extremos[k] & 31;
    paleta[k][0] = int((r << 3) | (r >> 2));
    paleta[k][1] = int((g << 2) | (g >> 4));
    paleta[k][2] = int((b << 3) | (b >> 2));
  }
  const bool tres = bc1 && extremos[0] <= extremos[1];
  for (int c = 0; c < 3; ++c) {
    paleta[2][c] = tres ? (paleta[0][c] + paleta[1][c]) / 2 : (2 * paleta[0][c] + paleta[1][c]) / 3;
    paleta[3][c] = tres ? 0 : (paleta[0][c] + 2 * paleta[1][c]) / 3;
  }
  uint8_t indice_bc[16];  // ETC order
  int cuenta[2][4] = {};
  for (int i = 0; i < 16; ++i) {
    indice_bc[i] = uint8_t((color[4 + (i & 3)] >> (2 * (i >> 2))) & 3);
    ++cuenta[(i >> 2) >= 2][indice_bc[i]];
  }
  const bool hay_transparentes = a1 && tres && (cuenta[0][3] || cuenta[1][3]);
  const int usados = hay_transparentes ? 3 : 4;  // index 3 is then transparent: no colour
  int media[2][3] = {}, total[2] = {};
  for (int s = 0; s < 2; ++s) {
    for (int k = 0; k < usados; ++k) {
      for (int c = 0; c < 3; ++c) media[s][c] += cuenta[s][k] * paleta[k][c];
      total[s] += cuenta[s][k];
    }
  }
  int q[2][3], base[2][3];
  for (int c = 0; c < 3; ++c) {
    for (int s = 0; s < 2; ++s) {
      const int fuente = total[s] ? s : total[1 - s] ? 1 - s : -1;
      media[s][c] = fuente < 0 ? 0 : (media[fuente][c] + total[fuente] / 2) / total[fuente];
    }
    q[0][c] = Cuantizar(media[0][c], 31);
    q[1][c] = q[0][c] + std::clamp(Cuantizar(media[1][c], 31) - q[0][c], -4, 3);
    base[0][c] = Expandir5(q[0][c]);
    base[1][c] = Expandir5(q[1][c]);
  }
  uint8_t indice_etc[2][4] = {};
  uint32_t tablas[2] = {};
  for (int s = 0; s < 2; ++s) {
    // The palette's offsets from the base (sum over R, G and B; the best modifier is about -s / 3), and the table
    // whose middle step is nearest their mean.
    int suma[4], desvio = 0;
    for (int k = 0; k < usados; ++k) {
      suma[k] = base[s][0] - paleta[k][0] + base[s][1] - paleta[k][1] + base[s][2] - paleta[k][2];
      desvio += cuenta[s][k] * std::abs(suma[k]);
    }
    const int objetivo = total[s] ? 2 * desvio / (3 * total[s]) : 0;  // in units of a + b
    int t = 0;
    while (t < 7 && std::abs(kEtc1[t + 1][0] + kEtc1[t + 1][1] - objetivo) <=
                        std::abs(kEtc1[t][0] + kEtc1[t][1] - objetivo)) {
      ++t;
    }
    tablas[s] = uint32_t(t);
    const int a = kEtc1[t][0], b = kEtc1[t][1];
    const int opacos[4] = {-b, -a, a, b}, indices_opacos[4] = {3, 2, 0, 1};
    const int sin_opacar[3] = {-b, 0, b}, indices_sin_opacar[3] = {3, 0, 1};
    const int* desplazamientos = hay_transparentes ? sin_opacar : opacos;
    const int* indices = hay_transparentes ? indices_sin_opacar : indices_opacos;
    const int n = hay_transparentes ? 3 : 4;
    for (int k = 0; k < usados; ++k) {
      int j = 0;
      while (j + 1 < n && 3 * (desplazamientos[j] + desplazamientos[j + 1]) < -2 * suma[k]) ++j;
      indice_etc[s][k] = uint8_t(indices[j]);
    }
    indice_etc[s][3] = hay_transparentes ? 2 : indice_etc[s][3];
  }
  uint32_t msb = 0, lsb = 0;
  for (int i = 0; i < 16; ++i) {
    const uint32_t indice = indice_etc[(i >> 2) >= 2][indice_bc[i]];
    msb |= (indice >> 1) << i;
    lsb |= (indice & 1) << i;
  }
  uint64_t bits = (uint64_t(tablas[0]) << 37) | (uint64_t(tablas[1]) << 34) |
                  (uint64_t(a1 ? !hay_transparentes : 1) << 33) | (uint64_t(msb) << 16) | lsb;
  for (int c = 0; c < 3; ++c) {
    bits |= (uint64_t(q[0][c]) << (59 - 8 * c)) | (uint64_t((q[1][c] - q[0][c]) & 7) << (56 - 8 * c));
  }
  Escribir64(bits, salida);
}

// A BC3 alpha, BC4 or BC5 channel block to EAC: the levels its texels use, each weighted by how many use it,
// encoded with table 15 (eight levels about evenly spaced; 13 when flat), then each texel's BC index through them.
inline void BorradorAlfa(const uint8_t* p, bool r11, uint8_t* salida) {
  int paleta[8] = {p[0], p[1]};  // as bc::Alfa
  if (p[0] > p[1]) {
    for (int i = 1; i <= 6; ++i) paleta[i + 1] = ((7 - i) * p[0] + i * p[1]) / 7;
  } else {
    for (int i = 1; i <= 4; ++i) paleta[i + 1] = ((5 - i) * p[0] + i * p[1]) / 5;
    paleta[6] = 0;
    paleta[7] = 255;
  }
  uint64_t indices_bc = 0;
  for (int i = 0; i < 6; ++i) indices_bc |= uint64_t(p[i + 2]) << (8 * i);
  int cuenta[8] = {}, objetivo[8], bajo = INT_MAX, alto = INT_MIN;
  for (int j = 0; j < 16; ++j) ++cuenta[(indices_bc >> (3 * j)) & 7];
  for (int k = 0; k < 8; ++k) {
    objetivo[k] = r11 ? (paleta[k] * 2047 + 127) / 255 : paleta[k];
    if (!cuenta[k]) continue;
    bajo = std::min(bajo, objetivo[k]);
    alto = std::max(alto, objetivo[k]);
  }
  const int maximo = r11 ? 2047 : 255;
  int64_t mejor_error = INT64_MAX;
  uint64_t cabecera = 0;
  uint8_t mapa[8] = {};
  const auto probar = [&](int tabla, int base, int mult) {
    int decodificado[8];
    for (int k = 0; k < 8; ++k) {
      const int m = kEac[tabla][kEacOrden[k]];
      decodificado[k] = r11 ? Saturar(base * 8 + 4 + (mult ? m * mult * 8 : m), maximo)
                            : Saturar(base + m * mult, maximo);
    }
    int64_t error = 0;
    uint8_t mapa_prueba[8] = {};
    for (int k = 0; k < 8; ++k) {
      if (!cuenta[k]) continue;
      int mejor = 0, e = INT_MAX;
      for (int j = 0; j < 8; ++j) {
        const int d = decodificado[j] - objetivo[k];
        if (d * d < e) {
          e = d * d;
          mejor = j;
        }
      }
      error += int64_t(e) * cuenta[k];
      mapa_prueba[k] = kEacOrden[mejor];
    }
    if (error >= mejor_error) return;
    mejor_error = error;
    cabecera = (uint64_t(base) << 56) | (uint64_t(mult) << 52) | (uint64_t(tabla) << 48);
    std::copy_n(mapa_prueba, 8, mapa);
  };
  if (bajo == alto) {
    probar(13, r11 ? bajo / 8 : bajo, r11 ? 0 : 1);
  } else {
    const int mn = kEac[15][3], mx = kEac[15][7];
    const double u_bajo = r11 ? (bajo - 4) / 8.0 : bajo, u_alto = r11 ? (alto - 4) / 8.0 : alto;
    const int mult = std::clamp(int(std::lround((u_alto - u_bajo) / (mx - mn))), 1, 15);
    probar(15, Saturar(int(std::lround((u_bajo + u_alto) / 2 - mult * (mn + mx) / 2.0)), 255), mult);
    if (r11 && alto - bajo <= 2 * (mx - mn)) {
      probar(15, Saturar(int(std::lround(((bajo + alto) / 2.0 - 4 - (mn + mx) / 2.0) / 8)), 255), 0);
    }
  }
  uint64_t indices = 0;
  for (int i = 0; i < 16; ++i) {  // ETC order; the BC indices are in row order
    indices = (indices << 3) | mapa[(indices_bc >> (3 * ((i & 3) * 4 + (i >> 2)))) & 7];
  }
  Escribir64(cabecera | indices, salida);
}

// ---- Whole textures ---------------------------------------------------------------------------------------------

// Block by block, in place of the RGBA8/R8/RG8 of bc::Convertir; the output has the input's size and layout.
// rapido: a draft to upload at once, while the full encode is done in the background (it is many times slower).
constexpr size_t kBytesBorradorEnHilos = 256 * 1024;  // below this, starting threads costs more than it saves
constexpr size_t kHilosBorrador = 4;                    // this thread and three more

inline bool Transcodificar(bc::Formato formato, std::span<const uint8_t> entrada, std::vector<uint8_t>& salida,
                           bool rapido = false, bool en_hilos = true) {
  if (uint8_t(formato) < 1 || uint8_t(formato) > 5) return false;
  const uint32_t bloque = bc::BytesBloque(formato), canales = bc::Canales(formato);
  if (entrada.size() % bloque) return false;
  salida.resize(entrada.size());
  // The draft runs on the ring thread when a texture streams in: on a Mali-G1 Ultra 3.5-10 MB in one frame took
  // 35-100 ms (the hitches when new scenery loads). Blocks are independent, so a large one is split over threads.
  if (rapido && en_hilos && entrada.size() >= kBytesBorradorEnHilos) {
    const size_t bloques = entrada.size() / bloque;
    const size_t partes = kHilosBorrador;
    std::thread hilos[kHilosBorrador - 1];
    bool ok[kHilosBorrador] = {};
    std::vector<uint8_t> salidas[kHilosBorrador];
    for (size_t k = 0; k < partes; ++k) {
      const size_t ini = bloques * k / partes * bloque, fin = bloques * (k + 1) / partes * bloque;
      const auto trozo = [&, k, ini, fin] {
        ok[k] = Transcodificar(formato, entrada.subspan(ini, fin - ini), salidas[k], true, false);
      };
      if (k + 1 < partes) {
        hilos[k] = std::thread(trozo);
      } else {
        trozo();
      }
    }
    bool todo = true;
    for (size_t k = 0; k < partes; ++k) {
      if (k + 1 < partes) hilos[k].join();
      const size_t ini = bloques * k / partes * bloque;
      todo = todo && ok[k];
      if (ok[k]) std::copy(salidas[k].begin(), salidas[k].end(), salida.begin() + ptrdiff_t(ini));
    }
    return todo;
  }
  for (size_t b = 0; b < entrada.size(); b += bloque) {
    const uint8_t* origen = entrada.data() + b;
    uint8_t* destino = salida.data() + b;
    if (rapido) {
      switch (formato) {
        case bc::Formato::BC1: BorradorColor(origen, true, true, destino); break;
        case bc::Formato::BC2: {
          // The 4-bit alpha as a BC4 block (255 to 0 in 8 steps) for the palette draft: Eac took twice as long as
          // the whole BC3 draft. Coarser, until the full encode replaces it.
          static constexpr uint8_t kNivel[16] = {1, 1, 7, 7, 6, 6, 5, 5, 4, 4, 3, 3, 2, 2, 0, 0};
          uint8_t bc4[8] = {255, 0};
          uint64_t indices = 0;
          for (int j = 0; j < 16; ++j) {
            indices |= uint64_t(kNivel[(origen[j / 2] >> (4 * (j % 2))) & 15]) << (3 * j);
          }
          for (int i = 0; i < 6; ++i) bc4[i + 2] = uint8_t(indices >> (8 * i));
          BorradorAlfa(bc4, false, destino);
          BorradorColor(origen + 8, false, false, destino + 8);
          break;
        }
        case bc::Formato::BC3:
          BorradorAlfa(origen, false, destino);
          BorradorColor(origen + 8, false, false, destino + 8);
          break;
        case bc::Formato::BC4: BorradorAlfa(origen, true, destino); break;
        case bc::Formato::BC5:
          BorradorAlfa(origen, true, destino);
          BorradorAlfa(origen + 8, true, destino + 8);
          break;
      }
      continue;
    }
    uint8_t decodificado[64];  // row order (y * 4 + x), `canales` per texel
    bc::Bloque(formato, origen, decodificado);
    if (canales < 4) {
      for (uint32_t c = 0; c < canales; ++c) {
        uint8_t valores[16];
        for (int i = 0; i < 16; ++i) valores[i] = decodificado[((i & 3) * 4 + (i >> 2)) * canales + c];
        Eac(valores, true, destino + 8 * c);
      }
      continue;
    }
    uint8_t rgb[16][3], alfa[16];
    uint16_t transparentes = 0;
    for (int i = 0; i < 16; ++i) {
      const uint8_t* p = decodificado + ((i & 3) * 4 + (i >> 2)) * 4;
      rgb[i][0] = p[0];
      rgb[i][1] = p[1];
      rgb[i][2] = p[2];
      alfa[i] = p[3];
      if (p[3] < 128) transparentes |= uint16_t(1u << i);
    }
    if (formato == bc::Formato::BC1) {
      Etc2(rgb, transparentes, true, destino);
    } else {
      Eac(alfa, false, destino);
      Etc2(rgb, 0, false, destino + 8);
    }
  }
  return true;
}
}  // namespace nfsmw::etc
