// Test driver for app/src/nativo/nfsmw_texturas_etc.h (run by tools/probar_etc.py, which checks the output with
// another decoder). For each of BC1-5 it builds a 256x256 texture: the top half random blocks (the worst case:
// any two endpoints, BC1 transparency), the bottom half blocks encoded from a synthetic picture (gradients, edges,
// flat areas). Writes <dir>/bcN.ref (bc::Convertir: RGBA8, R8 or RG8) and <dir>/bcN.etc (etc::Transcodificar),
// and <dir>/bcN.borrador (the draft), and prints the transcoding speed.
//
//   clang++ -std=c++20 -O2 -I app/src/nativo tools/probar_etc.cpp -o probar_etc && ./probar_etc <dir>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>

#include "nfsmw_texturas_etc.h"

namespace {
constexpr uint32_t kLado = 256;

void Imagen(uint32_t x, uint32_t y, uint8_t* rgba) {
  const int zona = int(x / 64);
  int r, g, b, a = 255;
  if (zona == 0) {  // smooth gradient
    r = int(x * 4) & 255; g = int(y * 2); b = 255 - int(y * 2);
  } else if (zona == 1) {  // two hues with a hard edge
    const bool lado = (x + y / 3) % 16 < 8;
    r = lado ? 220 : 30; g = lado ? 40 : 180; b = lado ? 30 : 60;
  } else if (zona == 2) {  // grey noise over flat
    r = g = b = 128 + int((x * 7919 + y * 104729) % 23) - 11;
  } else {  // cut-out: alpha on and off
    r = 200; g = 180; b = 20; a = ((x / 3 + y / 5) % 4) ? 255 : 0;
  }
  rgba[0] = uint8_t(r); rgba[1] = uint8_t(g); rgba[2] = uint8_t(b); rgba[3] = uint8_t(a);
}

uint16_t A565(const uint8_t* c) { return uint16_t(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)); }

// A simple encoder for the synthetic half: endpoints from the bounding box, nearest index.
void BloqueColor(const uint8_t (*p)[4], bool bc1, uint8_t* salida) {
  uint8_t mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
  bool transparente = false;
  for (int i = 0; i < 16; ++i) {
    if (bc1 && p[i][3] < 128) { transparente = true; continue; }
    for (int c = 0; c < 3; ++c) { mn[c] = std::min(mn[c], p[i][c]); mx[c] = std::max(mx[c], p[i][c]); }
  }
  uint16_t a = A565(mx), b = A565(mn);
  if (transparente ? a > b : a < b) std::swap(a, b);
  if (!transparente && a == b) { if (b) --b; else ++a; }
  salida[0] = uint8_t(a); salida[1] = uint8_t(a >> 8); salida[2] = uint8_t(b); salida[3] = uint8_t(b >> 8);
  uint8_t prueba[8] = {salida[0], salida[1], salida[2], salida[3], 0, 0, 0, 0};
  uint8_t tabla[64];
  nfsmw::bc::Bloque(nfsmw::bc::Formato::BC1, prueba, tabla);  // index 0 everywhere: tabla[0..3] is colour 0
  int colores[4][4];
  for (uint32_t k = 0; k < 4; ++k) {
    prueba[4] = uint8_t(k * 0x55);
    nfsmw::bc::Bloque(nfsmw::bc::Formato::BC1, prueba, tabla);
    for (int c = 0; c < 4; ++c) colores[k][c] = tabla[c];
  }
  for (int i = 0; i < 16; ++i) {
    int mejor = 0, e_mejor = 1 << 30;
    for (int k = 0; k < 4; ++k) {
      if (transparente && k == 3) continue;
      int e = 0;
      for (int c = 0; c < 3; ++c) e += (colores[k][c] - p[i][c]) * (colores[k][c] - p[i][c]);
      if (e < e_mejor) { e_mejor = e; mejor = k; }
    }
    if (bc1 && p[i][3] < 128) mejor = 3;
    salida[4 + i / 4] |= uint8_t(mejor << (2 * (i % 4)));
  }
}

void BloqueAlfa(const uint8_t* v, uint8_t* salida) {
  uint8_t mn = 255, mx = 0;
  for (int i = 0; i < 16; ++i) { mn = std::min(mn, v[i]); mx = std::max(mx, v[i]); }
  if (mn == mx) { if (mx < 255) ++mx; else --mn; }
  salida[0] = mx; salida[1] = mn;
  uint64_t indices = 0;
  for (int i = 0; i < 16; ++i) {
    // a0 > a1: palette 0 = a0, 1 = a1, 2..7 = (7 - k) / 7 steps from a0 to a1.
    const int paso = (int(mx - v[i]) * 7 + (mx - mn) / 2) / (mx - mn);
    const int indice = paso == 0 ? 0 : paso == 7 ? 1 : paso + 1;
    indices |= uint64_t(indice) << (3 * i);
  }
  for (int i = 0; i < 6; ++i) salida[2 + i] = uint8_t(indices >> (8 * i));
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: probar_etc <output dir>\n"); return 2; }
  const std::string dir = argv[1];
  std::mt19937 azar(1234);
  for (int f = 1; f <= 5; ++f) {
    const auto formato = nfsmw::bc::Formato(f);
    const uint32_t bytes = nfsmw::bc::BytesBloque(formato), bloques = kLado / 4;
    std::vector<uint8_t> bc(size_t(bloques) * bloques * bytes);
    for (uint32_t by = 0; by < bloques; ++by) for (uint32_t bx = 0; bx < bloques; ++bx) {
      uint8_t* b = bc.data() + (size_t(by) * bloques + bx) * bytes;
      if (by < bloques / 2) {
        for (uint32_t i = 0; i < bytes; ++i) b[i] = uint8_t(azar());
        continue;
      }
      uint8_t p[16][4];
      for (int i = 0; i < 16; ++i) Imagen(bx * 4 + i % 4, by * 4 + i / 4, p[i]);
      uint8_t canal[2][16];
      for (int i = 0; i < 16; ++i) { canal[0][i] = f == 4 || f == 5 ? p[i][0] : p[i][3]; canal[1][i] = p[i][1]; }
      if (f == 1) BloqueColor(p, true, b);
      if (f == 2) { for (int i = 0; i < 8; ++i) b[i] = uint8_t((p[2 * i][3] >> 4) | (p[2 * i + 1][3] & 0xF0)); }
      if (f == 3) BloqueAlfa(canal[0], b);
      if (f == 2 || f == 3) BloqueColor(p, false, b + 8);
      if (f == 4 || f == 5) BloqueAlfa(canal[0], b);
      if (f == 5) BloqueAlfa(canal[1], b + 8);
    }
    std::vector<uint8_t> ref, etc;
    std::array<uint32_t, 16> offsets{};
    constexpr int kVueltas = 8;
    auto antes = std::chrono::steady_clock::now();
    for (int v = 0; v < kVueltas; ++v) {
      if (!nfsmw::bc::Convertir(formato, bc, kLado, kLado, 1, 1, ref, offsets)) return 1;
    }
    const double s_bc = std::chrono::duration<double>(std::chrono::steady_clock::now() - antes).count();
    std::vector<uint8_t> borrador;
    antes = std::chrono::steady_clock::now();
    for (int v = 0; v < kVueltas; ++v) {
      if (!nfsmw::etc::Transcodificar(formato, bc, borrador, true)) return 1;
    }
    const double s_borrador = std::chrono::duration<double>(std::chrono::steady_clock::now() - antes).count();
    antes = std::chrono::steady_clock::now();
    for (int v = 0; v < kVueltas; ++v) {
      if (!nfsmw::etc::Transcodificar(formato, bc, etc)) return 1;
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - antes).count();
    const double texeles = kVueltas * double(kLado) * kLado / 1e6;
    std::printf("BC%d: Mtexel/s: ETC2/EAC %.1f, draft %.1f, decode to RGBA8/R8/RG8 %.1f\n", f, texeles / s,
                texeles / s_borrador, texeles / s_bc);
    std::ofstream(dir + "/bc" + std::to_string(f) + ".ref", std::ios::binary)
        .write(reinterpret_cast<const char*>(ref.data()), std::streamsize(ref.size()));
    std::ofstream(dir + "/bc" + std::to_string(f) + ".etc", std::ios::binary)
        .write(reinterpret_cast<const char*>(etc.data()), std::streamsize(etc.size()));
    std::ofstream(dir + "/bc" + std::to_string(f) + ".borrador", std::ios::binary)
        .write(reinterpret_cast<const char*>(borrador.data()), std::streamsize(borrador.size()));
  }
  return 0;
}
