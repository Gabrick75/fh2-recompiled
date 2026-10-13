// Prototype on ShiftGlue: the frame rate for the launcher's overlay and the game log, in FH2 Recompiled's format
// ("fps: ..." every 5 s, read by tools/ler_partida.sh), from ShiftGlue's count of guest frames (kSourceFrameCount).

#include <rex/logging.h>
#include <rex/perf/counter.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

extern std::atomic<float> g_fh2_nativo_fps;  // fh2_toque.cpp; read by android_rendimiento.cpp (nativeFps)
extern std::atomic<float> g_fh2_nativo_ms;

namespace fh2::medidor {

void Arrancar() {
  std::thread([] {
    using Clock = std::chrono::steady_clock;
    namespace perf = rex::perf;
    int64_t ultimo = perf::GetTotalCounter(perf::CounterId::kSourceFrameCount);
    int64_t inicio_ventana = ultimo;
    auto t_cambio = Clock::now(), t_ventana = Clock::now();
    double peor_ms = 0.0;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      const auto ahora = Clock::now();
      const int64_t cuenta = perf::GetTotalCounter(perf::CounterId::kSourceFrameCount);
      if (cuenta != ultimo) {
        // The longest gap between new frames, to 2 ms (the sampling step): the window's worst frame.
        peor_ms = std::max(peor_ms, std::chrono::duration<double, std::milli>(ahora - t_cambio).count());
        ultimo = cuenta;
        t_cambio = ahora;
      }
      const double segundos = std::chrono::duration<double>(ahora - t_ventana).count();
      if (segundos < 5.0) continue;
      const int64_t cuadros = cuenta - inicio_ventana;
      const double fps = cuadros / segundos;
      const double ms = cuadros ? segundos * 1000.0 / cuadros : 0.0;
      g_fh2_nativo_fps.store(float(fps), std::memory_order_relaxed);
      g_fh2_nativo_ms.store(float(ms), std::memory_order_relaxed);
      REXLOG_INFO("fps: {:.1f} ({} frames in {:.1f} s, {:.1f} ms average, worst {:.1f} ms, CPU do processador de "
                  "comandos -1.0 ms por quadro)",
                  fps, cuadros, segundos, ms, peor_ms);
      inicio_ventana = cuenta;
      t_ventana = ahora;
      peor_ms = 0.0;
    }
  }).detach();
}

}  // namespace fh2::medidor
