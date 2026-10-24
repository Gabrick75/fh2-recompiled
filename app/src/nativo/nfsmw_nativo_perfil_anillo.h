// Statistical profile of the ring thread (the thread that reads the game's GPU commands and records the Vulkan work).
//
// The per packet timers of the ring (tiempos por paquete, "anillo por Swap") time 1 packet in N and scale it up, which
// is far off for work as uneven as draws: most cost 3 us, the ones that open a render pass cost 100 times that. This
// looks at the thread instead: the ring marks which section it is in with one relaxed store (about 1 ns), and a sampler
// thread reads that mark every 0.5 ms and, every 10 s, logs the share of samples in each section ("perfil del anillo").
// Unbiased by construction, whatever the unevenness. Off unless fh2_nativo_perfil_anillo or fh2_nativo_medir_gpu is on.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/logging.h>

namespace nfsmw::perfil {

enum Seccion : uint8_t {
  kFuera = 0,     // outside packet handling: waiting for the game to write more, reports
  kPaquetes,      // a packet that none of the sections below names
  kRegistros,     // register writes
  kEsperaJuego,   // WAIT_REG_MEM: waiting for the game
  kCargaShader,   // IM_LOAD, IM_LOAD_IMMEDIATE: identifying a vertex or pixel shader
  kEmparejar,     // matching a draw with the game's Draw* record
  kPrepDibujo,    // building the draw request
  kDibujar,       // the renderer's per draw recording
  kTexturas,      // preparing a draw's textures
  kPaseAbrir,     // opening a render pass (framebuffer, render pass, begin)
  kPaseCerrar,    // closing a render pass
  kPipelineNuevo, // creating a pipeline
  kHacerCopia,    // a resolve / copy packet
  kCopiar,        // the renderer's copy and clear work
  kEnvio,         // submitting to the GPU
  kEsperaSubidas, // waiting for the vertex copy thread
  kSwap,          // the frame's swap
  kDiferidas,     // deferred fences and read-backs
  kNumero
};

inline constexpr const char* kNombres[kNumero] = {
    "fuera (esperando datos)", "otros paquetes",      "registros",         "WAIT_REG_MEM (juego)",
    "carga de shaders",        "emparejar dibujo",    "preparar dibujo",   "dibujar (grabar)",
    "texturas",                "abrir pase",          "cerrar pase",       "pipeline nuevo",
    "paquete de copia",        "copias y borrados",   "envio a la GPU",    "espera copias vertices",
    "swap",                    "diferidas"};

inline std::atomic<uint8_t> g_seccion{kFuera};
inline thread_local bool t_es_anillo = false;  // set by the ring thread: only it may move the mark

inline void Fijar(uint8_t seccion) {
  if (t_es_anillo) {
    g_seccion.store(seccion, std::memory_order_relaxed);
  }
}

// Section for the rest of a scope, then back to the one it interrupted.
class Alcance {
 public:
  explicit Alcance(uint8_t seccion) : activo_(t_es_anillo) {
    if (activo_) {
      anterior_ = g_seccion.load(std::memory_order_relaxed);
      g_seccion.store(seccion, std::memory_order_relaxed);
    }
  }
  ~Alcance() {
    if (activo_) {
      g_seccion.store(anterior_, std::memory_order_relaxed);
    }
  }
  Alcance(const Alcance&) = delete;
  Alcance& operator=(const Alcance&) = delete;

 private:
  bool activo_;
  uint8_t anterior_ = kFuera;
};

class Muestreador {
 public:
  static Muestreador& Instancia() {
    static Muestreador m;
    return m;
  }

  void Iniciar() {
    bool esperado = false;
    if (!iniciado_.compare_exchange_strong(esperado, true)) {
      return;
    }
    parar_.store(false);
    try {
      hilo_ = std::thread([this] { Bucle(); });
    } catch (...) {
      iniciado_.store(false);
    }
  }

  ~Muestreador() { Parar(); }

  void Parar() {
    parar_.store(true);
    if (hilo_.joinable()) {
      hilo_.join();
    }
    iniciado_.store(false);
  }

 private:
  void Bucle() {
    uint64_t cuentas[kNumero] = {};
    uint64_t total = 0;
    auto ultimo = std::chrono::steady_clock::now();
    while (!parar_.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::microseconds(500));
      ++cuentas[g_seccion.load(std::memory_order_relaxed) % kNumero];
      ++total;
      const auto ahora = std::chrono::steady_clock::now();
      if (ahora - ultimo < std::chrono::seconds(10)) {
        continue;
      }
      ultimo = ahora;
      std::vector<std::pair<uint64_t, int>> orden;
      for (int i = 0; i < kNumero; ++i) {
        if (cuentas[i]) {
          orden.emplace_back(cuentas[i], i);
        }
      }
      std::sort(orden.begin(), orden.end(), std::greater<>());
      const uint64_t ocupado = total - cuentas[kFuera];
      std::string texto;
      for (size_t i = 0; i < orden.size() && i < 12; ++i) {
        texto += fmt::format(" | {} {:.1f} %", kNombres[orden[i].second],
                             100.0 * double(orden[i].first) / double(total));
      }
      REXLOG_INFO("[fh2 nativo] perfil del anillo ({} muestras, ocupado {:.0f} %):{}", total,
                  100.0 * double(ocupado) / double(total ? total : 1), texto);
      for (uint64_t& c : cuentas) {
        c = 0;
      }
      total = 0;
    }
  }

  std::atomic<bool> iniciado_{false};
  std::atomic<bool> parar_{false};
  std::thread hilo_;
};

}  // namespace nfsmw::perfil
