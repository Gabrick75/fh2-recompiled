// CPU fallback for unsigned BC1-5, after untiling and guest endian conversion.
// GPU-independent so block decoding and mip/layer layout can be tested on PC.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace nfsmw::bc {
enum class Formato : uint8_t { BC1 = 1, BC2, BC3, BC4, BC5 };

inline uint32_t BytesBloque(Formato formato) {
  return formato == Formato::BC1 || formato == Formato::BC4 ? 8 : 16;
}

inline uint32_t Canales(Formato formato) {
  return formato == Formato::BC4 ? 1 : formato == Formato::BC5 ? 2 : 4;
}

inline uint16_t Leer16(const uint8_t* p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

inline void Alfa(const uint8_t* p, uint8_t* salida, uint32_t paso) {
  uint8_t tabla[8] = {p[0], p[1]};
  if (p[0] > p[1]) {
    for (uint32_t i = 1; i <= 6; ++i) tabla[i + 1] = uint8_t(((7 - i) * p[0] + i * p[1]) / 7);
  } else {
    for (uint32_t i = 1; i <= 4; ++i) tabla[i + 1] = uint8_t(((5 - i) * p[0] + i * p[1]) / 5);
    tabla[6] = 0;
    tabla[7] = 255;
  }
  uint64_t indices = 0;
  for (uint32_t i = 0; i < 6; ++i) indices |= uint64_t(p[i + 2]) << (8 * i);
  for (uint32_t i = 0; i < 16; ++i) salida[i * paso] = tabla[(indices >> (3 * i)) & 7];
}

inline void Bloque(Formato formato, const uint8_t* p, uint8_t* salida) {
  if (formato == Formato::BC4 || formato == Formato::BC5) {
    const uint32_t canales = Canales(formato);
    Alfa(p, salida, canales);
    if (formato == Formato::BC5) Alfa(p + 8, salida + 1, canales);
    return;
  }
  const uint8_t* color = formato == Formato::BC1 ? p : p + 8;
  const uint16_t a = Leer16(color), b = Leer16(color + 2);
  uint8_t tabla[4][4] = {};
  const auto rgb565 = [](uint16_t v, uint8_t* c) {
    const uint32_t r = v >> 11, g = (v >> 5) & 63, b = v & 31;
    c[0] = uint8_t((r << 3) | (r >> 2));
    c[1] = uint8_t((g << 2) | (g >> 4));
    c[2] = uint8_t((b << 3) | (b >> 2));
    c[3] = 255;
  };
  rgb565(a, tabla[0]);
  rgb565(b, tabla[1]);
  if (a > b || formato != Formato::BC1) {
    for (uint32_t c = 0; c < 3; ++c) {
      tabla[2][c] = uint8_t((2u * tabla[0][c] + tabla[1][c]) / 3);
      tabla[3][c] = uint8_t((tabla[0][c] + 2u * tabla[1][c]) / 3);
    }
    tabla[2][3] = tabla[3][3] = 255;
  } else {
    for (uint32_t c = 0; c < 3; ++c) tabla[2][c] = uint8_t((uint32_t(tabla[0][c]) + tabla[1][c]) / 2);
    tabla[2][3] = 255;  // index 3 stays transparent black in BC1.
  }
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t indice = (color[4 + i / 4] >> (2 * (i % 4))) & 3;
    std::copy_n(tabla[indice], 4, salida + i * 4);
    if (formato == Formato::BC2) salida[i * 4 + 3] = uint8_t(((p[i / 2] >> (4 * (i % 2))) & 15) * 17);
  }
  if (formato == Formato::BC3) Alfa(p, salida + 3, 4);
}

// Input and output: each mip contains all layers consecutively. Tiny mips
// still occupy a full BC block on input, but only their texels on output.
// Offsets are committed only on success, so a bad upload cannot be queued.
inline bool Convertir(Formato formato, std::span<const uint8_t> entrada, uint32_t ancho,
                      uint32_t alto, uint32_t capas, uint32_t niveles,
                      std::vector<uint8_t>& salida, std::array<uint32_t, 16>& offsets) {
  if (uint8_t(formato) < 1 || uint8_t(formato) > 5 || !ancho || !alto || !capas ||
      !niveles || niveles > offsets.size()) return false;
  const uint32_t canales = Canales(formato), bloque_bytes = BytesBloque(formato);
  uint64_t bytes_entrada = 0, bytes_salida = 0;
  std::array<uint32_t, 16> nuevos{};
  for (uint32_t n = 0; n < niveles; ++n) {
    const uint64_t w = std::max(ancho >> n, 1u), h = std::max(alto >> n, 1u);
    const uint64_t plano_in = ((w + 3) / 4) * ((h + 3) / 4) * bloque_bytes;
    const uint64_t limite = std::numeric_limits<uint32_t>::max();
    if (w > limite / canales / h) return false;
    const uint64_t plano_out = w * h * canales;
    // Renderer offsets and upload accounting are 32-bit. Check before
    // multiplying by layers, including malformed huge dimensions.
    bytes_salida = (bytes_salida + 3) & ~uint64_t(3);  // VkBufferImageCopy bufferOffset alignment
    if (bytes_salida > limite || plano_in > (limite - bytes_entrada) / capas ||
        plano_out > (limite - bytes_salida) / capas) return false;
    nuevos[n] = uint32_t(bytes_salida);
    bytes_entrada += plano_in * capas;
    bytes_salida += plano_out * capas;
  }
  if (bytes_entrada != entrada.size()) return false;
  salida.resize(size_t(bytes_salida));
  size_t origen = 0;
  for (uint32_t n = 0; n < niveles; ++n) {
    const uint32_t w = std::max(ancho >> n, 1u), h = std::max(alto >> n, 1u);
    const uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    const size_t plano_out = size_t(w) * h * canales;
    if (n) {
      const size_t fin_anterior = size_t(nuevos[n - 1]) +
          size_t(std::max(ancho >> (n - 1), 1u)) * std::max(alto >> (n - 1), 1u) * canales * capas;
      std::fill(salida.begin() + fin_anterior, salida.begin() + nuevos[n], 0);
    }
    for (uint32_t capa = 0; capa < capas; ++capa) {
      uint8_t* plano = salida.data() + nuevos[n] + plano_out * capa;
      for (uint32_t y = 0; y < by; ++y) for (uint32_t x = 0; x < bx; ++x) {
        uint8_t pixeles[64];
        Bloque(formato, entrada.data() + origen, pixeles);
        origen += bloque_bytes;
        for (uint32_t fila = 0; fila < std::min(4u, h - y * 4); ++fila) {
          std::copy_n(pixeles + fila * 4 * canales, std::min(4u, w - x * 4) * canales,
                      plano + (size_t(y * 4 + fila) * w + x * 4) * canales);
        }
      }
    }
  }
  offsets = nuevos;
  return true;
}
}  // namespace nfsmw::bc
