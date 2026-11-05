// fh2 - the main view's aspect ratio ("tela" means "screen"): fill a non-16:9 screen without distortion.
//
// sub_823F2290 is the only function in default.xex that returns a viewport's width / height (r3 = viewport, width
// at +8, height at +12, the ratio in f1). The camera, its culling frustum and its projection take the aspect from
// it, so returning a larger ratio for the 1280x720 main view makes the game draw (and cull) a wider horizontal
// field into the same 16:9 image; a presentation stretched to the screen then shows it undistorted. Forza Horizon
// 1 has the same function (sub_823E22C8, referencias/pinyon-shift), where it is hooked for the same purpose.
//
// It has seven call sites. Logged on the PC (fh2_tela_registro, 2026-11-05) and tried with the ratio x1.3:
//   8262CCF0, 8262D070 (sub_8262CC40), 8244E280, 82B2420C  1280x720 once a frame in the open world; widened together
//                                     the world draws wider with nothing missing at the edges. 82B2420C almost stops
//                                     (1.8/s) when the pause map is open: the gameplay camera.
//   82789008                          256x256 in the open world (the minimap) and 1280x720 with the big map open.
//                                     Widened, the big map came out with its edges cut: left alone.
//   82A35000, 828EC844                never called.
// The HUD does not go through this function (it kept its shape on the PC).
//
// fh2_tela_aspecto is the screen's width / height when the picture is stretched to it (GameActivity sends it; 0 or
// 16:9 = off); the world's sites get the ratio times aspecto / (16/9). fh2_tela_fator (1 = off) replaces that factor,
// for tests, and fh2_tela_sitios replaces the sites (hex return addresses separated by commas).
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <fmt/format.h>

REXCVAR_DEFINE_BOOL(fh2_tela_registro, true, "FH2",
                    "Debug: log the calls of the viewport aspect function (sub_823F2290) per call site and the "
                    "viewport sizes (app/src/fh2_tela.cpp)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(fh2_tela_registro_s, 2.0, "FH2", "Debug: seconds between the [tela] reports");
REXCVAR_DEFINE_DOUBLE(fh2_tela_aspecto, 0.0, "FH2",
                      "Width / height of the screen the picture is stretched to (0 = not stretched): the world is "
                      "drawn with that aspect into the 16:9 image, so the stretch does not distort it "
                      "(app/src/fh2_tela.cpp)")
    .range(0.0, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(fh2_tela_fator, 1.0, "FH2",
                      "Test: multiply the world's 1280x720 aspect ratio by this instead of the one fh2_tela_aspecto "
                      "gives (1 = not used)")
    .range(0.5, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(fh2_tela_hud, true, "FH2",
                    "Keep the HUD at 16:9 while the world is widened for a stretched screen (the native renderer "
                    "squeezes its draws sideways around the centre)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(fh2_tela_sitios, "8262CCF0,8262D070,8244E280,82B2420C", "FH2",
                      "Call sites of sub_823F2290 that are widened (hex return addresses separated by commas; empty = "
                      "all): the world's, not the big map's 82789008")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

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

// Diagnostics: calls to the loading screens' own methods (ForzaUI::CDynamicTipLoadingScreen vtable 8206813C,
// CDynamicTipLoadingScreenFunctional 82068414, CNonFunctionalLoadingScreen 820749C4: the entries that differ from the
// base class), to find one the game calls every frame while a loading screen is up.
constexpr std::array<uint32_t, 10> kMetodosCarga = {0x828133C8, 0x8280DE70, 0x82815658, 0x8280E0A0, 0x82815808,
                                                    0x827F56A8, 0x827F09C0, 0x8287F628, 0x8287D960, 0x82882268};
std::array<std::atomic<uint64_t>, kMetodosCarga.size()> g_metodos_carga{};

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
  std::string carga;
  for (size_t i = 0; i < kMetodosCarga.size(); ++i) {
    if (const uint64_t n = g_metodos_carga[i].exchange(0, std::memory_order_relaxed)) {
      carga += fmt::format(" {:08X}={}", kMetodosCarga[i], n);
    }
  }
  if (!carga.empty()) texto += " | carga" + carga;
  if (const uint64_t otros = g_otros.load(std::memory_order_relaxed)) {
    texto += fmt::format(" | otros sitios {}", otros);
  }
  REXLOG_INFO("[tela] {:.1f} s, aspecto {:.4f}, fator {:.3f}{}", segundos, REXCVAR_GET(fh2_tela_aspecto),
              REXCVAR_GET(fh2_tela_fator),
              texto.empty() ? " | sin llamadas" : texto);
}

// The factor last applied to the gameplay camera (82B2420C) and when, for the HUD.
constexpr uint32_t kSitioCamara = 0x82B2420C;
std::atomic<float> g_fator_mundo{1.0f};
std::atomic<int64_t> g_mundo_ms{0};

int64_t AhoraMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Whether this call site is widened (fh2_tela_sitios, read once: the list needs a restart).
bool SitioElegido(uint32_t retorno) {
  static const std::vector<uint32_t> sitios = [] {
    std::vector<uint32_t> lista;
    const std::string texto = REXCVAR_GET(fh2_tela_sitios);
    size_t inicio = 0;
    while (inicio < texto.size()) {
      size_t fin = texto.find(',', inicio);
      if (fin == std::string::npos) fin = texto.size();
      const std::string pieza = texto.substr(inicio, fin - inicio);
      if (!pieza.empty()) lista.push_back(static_cast<uint32_t>(std::stoul(pieza, nullptr, 16)));
      inicio = fin + 1;
    }
    REXLOG_INFO("[tela] sitios: {}", lista.empty() ? "todos" : texto);
    return lista;
  }();
  return sitios.empty() || std::find(sitios.begin(), sitios.end(), retorno) != sitios.end();
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
  double fator = REXCVAR_GET(fh2_tela_fator);
  if (fator == 1.0) {
    const double aspecto = REXCVAR_GET(fh2_tela_aspecto);
    if (aspecto > 0.0) fator = aspecto / (16.0 / 9.0);
  }
  if (std::abs(fator - 1.0) > 0.001 && ancho == 1280 && alto == 720 && SitioElegido(retorno)) {
    ctx.f1.f64 = double(float(ctx.f1.f64 * fator));
    if (retorno == kSitioCamara) {
      g_fator_mundo.store(float(fator), std::memory_order_relaxed);
      g_mundo_ms.store(AhoraMs(), std::memory_order_relaxed);
    }
  }
}

// For the native renderer (nfsmw_nativo_dibujos.cpp): the HUD's squeeze, the world's factor while the gameplay
// camera was widened in the last quarter of a second (it almost stops with the big map open), else 1.
float Fh2TelaFactorHud() {
  if (!REXCVAR_GET(fh2_tela_hud)) return 1.0f;
  if (AhoraMs() - g_mundo_ms.load(std::memory_order_relaxed) > 250) return 1.0f;
  return g_fator_mundo.load(std::memory_order_relaxed);
}

// The loading screens' methods, counted (kMetodosCarga).
#define FH2_TELA_METODO_CARGA(indice, nombre)                                    \
  REX_EXTERN(__imp__##nombre);                                                  \
  REX_HOOK_RAW(nombre) {                                                        \
    g_metodos_carga[indice].fetch_add(1, std::memory_order_relaxed);            \
    __imp__##nombre(ctx, base);                                                 \
  }
FH2_TELA_METODO_CARGA(0, sub_828133C8)
FH2_TELA_METODO_CARGA(1, sub_8280DE70)
FH2_TELA_METODO_CARGA(2, sub_82815658)
FH2_TELA_METODO_CARGA(3, sub_8280E0A0)
FH2_TELA_METODO_CARGA(4, sub_82815808)
FH2_TELA_METODO_CARGA(5, sub_827F56A8)
FH2_TELA_METODO_CARGA(6, sub_827F09C0)
FH2_TELA_METODO_CARGA(7, sub_8287F628)
FH2_TELA_METODO_CARGA(8, sub_8287D960)
FH2_TELA_METODO_CARGA(9, sub_82882268)
#undef FH2_TELA_METODO_CARGA
