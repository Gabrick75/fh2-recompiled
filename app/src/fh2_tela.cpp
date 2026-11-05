// fh2 - the main view's aspect ratio ("tela" means "screen"), step 1 of filling non-16:9 screens without distortion.
//
// sub_823F2290 is the only function in default.xex that returns a viewport's width / height (r3 = viewport, width
// at +8, height at +12, the ratio in f1). The camera, its culling frustum and its projection take the aspect from
// it, so returning a larger ratio for the 1280x720 main view makes the game draw (and cull) a wider horizontal
// field into the same 16:9 image; a presentation stretched to the screen then shows it undistorted. Forza Horizon
// 1 has the same function (sub_823E22C8, referencias/pinyon-shift), where it is hooked for the same purpose.
//
// It has seven call sites: 8244E280, 82A35000, 828EC844, 82789008, 82B2420C, 8262CCF0 and 8262D070 (the last two in
// sub_8262CC40). Which of them is the gameplay camera, the pause map, the menus or the render-to-texture views is
// not known yet: fh2_tela_registro logs, every few seconds, the calls per site and the viewport sizes each one saw.
//
// fh2_tela_fator (1 = off) multiplies the ratio of every 1280x720 call, from any site: a test to see the hook work
// (on a 16:9 window the picture looks squeezed sideways, with more of the scene at the edges).
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include <fmt/format.h>

REXCVAR_DEFINE_BOOL(fh2_tela_registro, false, "FH2",
                    "Debug: log the calls of the viewport aspect function (sub_823F2290) per call site and the "
                    "viewport sizes (app/src/fh2_tela.cpp)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(fh2_tela_registro_s, 5.0, "FH2", "Debug: seconds between the [tela] reports");
REXCVAR_DEFINE_DOUBLE(fh2_tela_fator, 1.0, "FH2",
                      "Test: multiply the aspect ratio of every 1280x720 viewport by this (1 = off), so the game "
                      "draws a wider horizontal field into its 16:9 image (app/src/fh2_tela.cpp)")
    .range(0.5, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_823F2290);

namespace {

uint32_t CargarU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  __builtin_memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

// Per call site: calls since the last report and up to four distinct viewport sizes (width << 16 | height).
struct Sitio {
  std::atomic<uint32_t> retorno{0};
  std::atomic<uint64_t> llamadas{0};
  std::atomic<uint64_t> total{0};
  std::array<std::atomic<uint32_t>, 4> tamanos{};
  std::array<std::atomic<uint64_t>, 4> por_tamano{};
};
constexpr size_t kSitios = 16;
std::array<Sitio, kSitios> g_sitios;
std::atomic<uint64_t> g_otros{0};

Sitio* BuscarSitio(uint32_t retorno) {
  for (auto& s : g_sitios) {
    uint32_t actual = s.retorno.load(std::memory_order_relaxed);
    if (actual == retorno) return &s;
    if (actual == 0 && s.retorno.compare_exchange_strong(actual, retorno)) return &s;
    if (actual == retorno) return &s;  // another thread took this slot for the same site
  }
  return nullptr;
}

void Anotar(uint32_t retorno, uint32_t ancho, uint32_t alto) {
  Sitio* s = BuscarSitio(retorno);
  if (!s) {
    g_otros.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  s->llamadas.fetch_add(1, std::memory_order_relaxed);
  s->total.fetch_add(1, std::memory_order_relaxed);
  const uint32_t clave = (ancho & 0xFFFF) << 16 | (alto & 0xFFFF);
  for (size_t i = 0; i < s->tamanos.size(); ++i) {
    uint32_t actual = s->tamanos[i].load(std::memory_order_relaxed);
    if (actual == 0 && s->tamanos[i].compare_exchange_strong(actual, clave)) actual = clave;
    if (actual == clave) {
      s->por_tamano[i].fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}

void Informar() {
  using Reloj = std::chrono::steady_clock;
  static std::atomic<int64_t> ultimo{0};
  const int64_t ahora =
      std::chrono::duration_cast<std::chrono::milliseconds>(Reloj::now().time_since_epoch()).count();
  int64_t previo = ultimo.load(std::memory_order_relaxed);
  const auto intervalo = static_cast<int64_t>(REXCVAR_GET(fh2_tela_registro_s) * 1000.0);
  if (previo == 0) {
    ultimo.compare_exchange_strong(previo, ahora);
    return;
  }
  if (ahora - previo < intervalo || !ultimo.compare_exchange_strong(previo, ahora)) return;
  const double segundos = double(ahora - previo) / 1000.0;
  std::string texto;
  for (auto& s : g_sitios) {
    const uint32_t retorno = s.retorno.load(std::memory_order_relaxed);
    if (retorno == 0) break;
    const uint64_t n = s.llamadas.exchange(0, std::memory_order_relaxed);
    texto += fmt::format(" | {:08X}: {:.1f}/s (total {})", retorno, double(n) / segundos,
                         s.total.load(std::memory_order_relaxed));
    for (size_t i = 0; i < s.tamanos.size(); ++i) {
      const uint32_t clave = s.tamanos[i].load(std::memory_order_relaxed);
      if (clave == 0) break;
      const uint64_t m = s.por_tamano[i].exchange(0, std::memory_order_relaxed);
      if (m) texto += fmt::format(" {}x{}={}", clave >> 16, clave & 0xFFFF, m);
    }
  }
  if (const uint64_t otros = g_otros.load(std::memory_order_relaxed)) {
    texto += fmt::format(" | otros sitios {}", otros);
  }
  REXLOG_INFO("[tela] {:.1f} s, fator {:.3f}{}", segundos, REXCVAR_GET(fh2_tela_fator),
              texto.empty() ? " | sin llamadas" : texto);
}

}  // namespace

REX_HOOK_RAW(sub_823F2290) {
  const uint32_t vista = ctx.r3.u32;
  const uint32_t retorno = static_cast<uint32_t>(ctx.lr);
  __imp__sub_823F2290(ctx, base);
  const uint32_t ancho = CargarU32(base, vista + 8), alto = CargarU32(base, vista + 12);
  if (REXCVAR_GET(fh2_tela_registro)) {
    Anotar(retorno, ancho, alto);
    Informar();
  }
  const double fator = REXCVAR_GET(fh2_tela_fator);
  if (fator != 1.0 && ancho == 1280 && alto == 720) {
    ctx.f1.f64 = double(float(ctx.f1.f64 * fator));
  }
}
