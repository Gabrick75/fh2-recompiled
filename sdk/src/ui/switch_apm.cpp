/*
 * nfsmw - console performance profile (apm).
 *
 * Why
 *   The GPU runs at 307.2 MHz in handheld mode and it is one of the two walls of the port (the other
 *   is the CPU, with three cores at 85-99 %). Nintendo later released the 460.8 MHz handheld profile
 *   precisely for games that need it, and this one does: 25 FPS with the GPU at its limit. It is not
 *   an overclock: no KIP or sysmodule is touched, it is requested through Horizon's official API
 *   (apm), the same one any commercial game uses. That is why not every game requests it: those that
 *   do not need it get longer battery life.
 *
 *   307.2 -> 460.8 MHz is +50 % of GPU.
 *
 * Memory is not touched
 *   The first version raised the memory from 1331.2 to 1597.5 MHz by accident: apm does not take MHz,
 *   it takes a configuration number, and the high-GPU configurations come in two flavors, with EMC
 *   1331.2 and with EMC 1600. The 1600 one got in. A clock overlay showed it, and a commercial game
 *   runs at GPU 460.8 with MEM 1331.2: so that combination exists and it is the one we want.
 *
 *   Raising the memory clock uses more battery and heat without giving us anything: our own
 *   measurements say bandwidth is at 27 % and what saturates is the fragment ALU.
 *
 * The three rounds it took, and what each one taught
 *
 *   1. It applied the configuration and read the clocks on the next line. The log said "la
 *      memoria se queda en 1331.2" and the session profile recorded 1600.0 in every sample. They
 *      did not contradict each other: it was checked too early. apm hands the change to pcv and
 *      the clocks are applied later, so the immediate read returned the old value. The same bug
 *      left dead a fix that only lowered the EMC by hand "if it is detected that it went up": it
 *      was never detected.
 *
 *   2. A 250 ms wait plus a once-per-second watchdog that lowered the EMC with clkrst. The log said
 *      the two things that needed knowing:
 *        - after 250 ms the memory was still at 1331.2 (so the raise takes between 0.25 and ~1 s,
 *          and 250 ms was not enough either);
 *        - the watchdog found it at 1600 and lowered it eight times with clkrst; all eight were
 *          accepted and all eight were reverted by the system. pcv reimposes the EMC of the active
 *          apm configuration, and clkrst cannot fight that.
 *
 *   3. So the problem was never how to lower the memory, but which configuration is requested.
 *      0x92220007 is "GPU 460.8 + EMC 1600", and its sibling 0x92220008 is "GPU 460.8 + EMC
 *      1331.2": that is the right one, and it was missing from the list (a nonexistent 0x92220006
 *      was there instead). Now:
 *        1. the table has the real IDs, sorted with the low EMC first;
 *        2. the check is a 1.5 s poll (MemoriaQuietaDurante), not a single read;
 *        3. the watchdog stays as a safety net, in case the system reimposes the EMC on a mode
 *           change or when waking from sleep.
 *
 *   It only ever lowers back to what the console had at the start: nothing is ever raised above
 *   that. And if this firmware did not have the "high GPU + unchanged RAM" pair, the GPU is left
 *   as it was rather than raising the RAM; nfsmw_switch_ram_1600 opts into that trade.
 */

#include "rex/ui/switch_apm.h"

#if REX_PLATFORM_SWITCH

#include <switch.h>

#include <atomic>
#include <cstdio>

namespace {

std::atomic<int> g_pedido{0};
std::atomic<bool> g_hecho{false};
/*
 * The escape hatch. By default no configuration that raises the RAM clock is accepted, even if that
 * means staying with the low GPU clock. If the firmware turned out not to have the "high GPU +
 * unchanged RAM" pair, setting this to true takes the RAM 1600 one anyway.
 */
std::atomic<bool> g_permitir_ram_1600{false};

/*
 * Watchdog state. g_emc_original is the memory clock the console had before we touched anything, in
 * Hz, and it is 0 while we have not changed the configuration: with 0 the watchdog does nothing at
 * all (it does not even open clkrst).
 */
std::atomic<u32> g_emc_original{0};
std::atomic<int> g_tics_emc{0};
std::atomic<int> g_correcciones_emc{0};
std::atomic<bool> g_aviso_emc{false};
std::atomic<bool> g_rendido_emc{false};

/*
 * How often the watchdog looks. For the first kTicsSeguidos ticks (one second each) it always looks,
 * which is when the system finishes applying the change (the profile already saw 1600 in its first
 * sample, at 8 s). From then on, one of every kTicsEspaciados, to catch late raises (mode change,
 * waking from sleep) without opening clkrst every second for the whole session.
 */
constexpr int kTicsSeguidos = 20;
constexpr int kTicsEspaciados = 30;
/*
 * And a cap on corrections. Measured: eight clkrst reductions were all accepted and all reverted by
 * pcv, which reimposes the EMC of the active apm configuration. With the corrected table this should
 * never fire; if it does, the chosen configuration is not the one we thought, and insisting fixes
 * nothing. It is logged and left alone.
 */
constexpr int kMaxCorreccionesEmc = 3;

/*
 * What is watched after requesting the configuration, before accepting anything.
 *
 * Waiting 250 ms was still not enough: the log said "la memoria se queda en 1331.2 MHz (comprobado
 * 250 ms despues)" and one second later the watchdog already found it at 1600. So the raise takes
 * between 0.25 and ~1 s. Now it polls every 150 ms for 1.5 s, and a single read that sees it moved
 * is enough to reject the configuration. It only costs that second and a half when the candidate is
 * good; when it is bad it stops as soon as it shows.
 */
constexpr u64 kSondeoPasoNs = 150000000ULL;
constexpr int kSondeoPasos = 10;  // 10 x 150 ms = 1,5 s
constexpr u32 kMargenHz = 20000000u;  // 20 MHz: 1597.5 and 1331.2 are nowhere near each other

/*
 * Real hardware clocks, in Hz. Returns false if clkrst is not available. It is opened and closed on
 * every call on purpose: this runs only a few times per session.
 */
bool LeerRelojes(u32& gpu_hz, u32& emc_hz) {
  gpu_hz = 0;
  emc_hz = 0;
  if (R_FAILED(clkrstInitialize())) {
    return false;
  }
  bool ok = true;
  ClkrstSession ses_gpu{};
  ClkrstSession ses_emc{};
  if (R_SUCCEEDED(clkrstOpenSession(&ses_gpu, PcvModuleId_GPU, 3))) {
    if (R_FAILED(clkrstGetClockRate(&ses_gpu, &gpu_hz))) ok = false;
    clkrstCloseSession(&ses_gpu);
  } else {
    ok = false;
  }
  if (R_SUCCEEDED(clkrstOpenSession(&ses_emc, PcvModuleId_EMC, 3))) {
    if (R_FAILED(clkrstGetClockRate(&ses_emc, &emc_hz))) ok = false;
    clkrstCloseSession(&ses_emc);
  } else {
    ok = false;
  }
  clkrstExit();
  return ok;
}

/*
 * Polls the clocks for kSondeoPasos x kSondeoPasoNs and returns true if the memory clock stayed where
 * it was the whole time. As soon as one read sees it moved, it stops and returns false, leaving the
 * last values read in gpu/emc. A single read is not enough: one read at 250 ms said the memory was
 * unchanged when it was about to go up a moment later.
 */
bool MemoriaQuietaDurante(u32 emc0, u32& gpu, u32& emc) {
  for (int i = 0; i < kSondeoPasos; ++i) {
    svcSleepThread(kSondeoPasoNs);
    if (!LeerRelojes(gpu, emc)) {
      return false;
    }
    if ((emc > emc0 ? emc - emc0 : emc0 - emc) > kMargenHz) {
      return false;
    }
  }
  return true;
}

/*
 * Sets the memory module clock to hz. Returns whether the request was accepted (not whether the clock
 * already changed: that is checked later, which is exactly the lesson of the first attempt).
 */
bool FijarMemoria(u32 hz) {
  bool ok = false;
  if (R_SUCCEEDED(clkrstInitialize())) {
    ClkrstSession ses{};
    if (R_SUCCEEDED(clkrstOpenSession(&ses, PcvModuleId_EMC, 3))) {
      ok = R_SUCCEEDED(clkrstSetClockRate(&ses, hz));
      clkrstCloseSession(&ses);
    }
    clkrstExit();
  }
  return ok;
}

struct Candidata {
  int mhz;
  uint32_t config;
  int emc;  // log only: what Horizon's table says this configuration sets
};
/*
 * The correct table. The first two are confirmed by a console log:
 *
 *   0x00020003  GPU 307.2  EMC 1331.2   <- "de partida: config 0x00020003 ... memoria 1331.2"
 *   0x92220007  GPU 460.8  EMC 1600     <- "PUESTA 0x92220007" and the memory ended at 1600
 *
 * and they match Horizon's PerformanceConfiguration table exactly. The one we want is the sibling of
 * the last one, 0x92220008 = GPU 460.8 with EMC 1331.2, which is exactly the pair a commercial game
 * uses. An earlier list lacked it (it had 0x92220006, which does not exist), so the 1600 one always
 * got in.
 *
 * They are sorted by EMC: first the one that does not touch the memory. Requesting one that does not
 * exist returns an error and changes nothing, so it is enough to list them and let the check decide.
 */
constexpr Candidata kCandidatas[] = {
    {460, 0x92220008u, 1331},  // GPU 460,8 + EMC 1331,2: la buena
    {460, 0x92220007u, 1600},  // GPU 460.8 + EMC 1600: raises the RAM clock
    {384, 0x00020004u, 1331},  // GPU 384 + EMC 1331,2
    {384, 0x00010000u, 1600},  // GPU 384 + EMC 1600
};


}  // namespace

extern "C" void RexSwitchApmPedirGpuMhz(int mhz) {
  g_pedido.store(mhz, std::memory_order_relaxed);
}

extern "C" void RexSwitchApmPermitirRam1600(int permitir) {
  g_permitir_ram_1600.store(permitir != 0, std::memory_order_relaxed);
}

extern "C" void RexSwitchApmAplicar(void) {
  const int mhz = g_pedido.load(std::memory_order_relaxed);
  if (mhz <= 0 || g_hecho.exchange(true)) {
    return;
  }

  ApmPerformanceMode modo = ApmPerformanceMode_Invalid;
  if (R_FAILED(apmGetPerformanceMode(&modo))) {
    std::fprintf(stderr, "[apm] no se pudo leer el modo de rendimiento; no se toca nada\n");
    return;
  }
  if (modo != ApmPerformanceMode_Normal) {
    std::fprintf(stderr, "[apm] la consola esta en sobremesa (modo %d): no se pide nada\n", int(modo));
    return;
  }

  uint32_t config_original = 0;
  const bool hay_original = R_SUCCEEDED(apmGetPerformanceConfiguration(modo, &config_original));

  u32 gpu0 = 0, emc0 = 0;
  if (!LeerRelojes(gpu0, emc0)) {
    std::fprintf(stderr,
                 "[apm] no se pueden leer los relojes reales (clkrst): NO se cambia nada, porque sin "
                 "comprobar no se puede garantizar que la memoria se quede donde esta\n");
    return;
  }
  std::fprintf(stderr, "[apm] de partida: config 0x%08X, GPU %.1f MHz, memoria %.1f MHz\n",
               hay_original ? config_original : 0u, double(gpu0) / 1e6, double(emc0) / 1e6);

  const u32 objetivo_gpu_hz = u32(mhz) * 1000000u;

  /*
   * They are tried in table order (low EMC first) and the first one that raises the GPU without
   * moving the memory is accepted. The check is the 1.5 s poll, not a single read: a read at 250 ms
   * once said the memory was still at 1331.2, and one second later it was at 1600.
   */
  for (const Candidata& c : kCandidatas) {
    if (c.mhz != mhz) {
      continue;
    }
    if (R_FAILED(apmSetPerformanceConfiguration(modo, c.config))) {
      continue;  // that configuration does not exist on this firmware
    }
    u32 gpu1 = 0, emc1 = 0;
    const bool quieta_de_verdad = MemoriaQuietaDurante(emc0, gpu1, emc1);
    const bool memoria_quieta =
        quieta_de_verdad || g_permitir_ram_1600.load(std::memory_order_relaxed);
    const bool gpu_subio = gpu1 + kMargenHz >= objetivo_gpu_hz;
    if (gpu_subio && memoria_quieta) {
      if (!quieta_de_verdad) {
        std::fprintf(stderr,
                     "[apm] 0x%08X sube la memoria a %.1f MHz, pero nfsmw_switch_ram_1600 esta en "
                     "true: se acepta y NO se vigila la memoria\n",
                     c.config, double(emc1) / 1e6);
        std::fprintf(stderr, "[apm] PUESTA 0x%08X: GPU %.1f MHz (era %.1f), memoria %.1f MHz\n",
                     c.config, double(gpu1) / 1e6, double(gpu0) / 1e6, double(emc1) / 1e6);
        return;  // watchdog not armed: lowering it would fight what the cvar asks for
      }
      /*
       * Accepted. The watchdog is armed anyway: the system may reimpose the EMC later,
       * on a mode change or when waking from sleep.
       */
      g_emc_original.store(emc0, std::memory_order_relaxed);
      std::fprintf(stderr,
                   "[apm] PUESTA la configuracion 0x%08X (tabla: GPU %d, EMC %d): GPU %.1f MHz (era "
                   "%.1f) y la memoria se queda en %.1f MHz, comprobada durante %.1f s seguidos; "
                   "queda vigilada\n",
                   c.config, c.mhz, c.emc, double(gpu1) / 1e6, double(gpu0) / 1e6,
                   double(emc1) / 1e6, double(kSondeoPasoNs) * kSondeoPasos / 1e9);
      return;
    }
    std::fprintf(stderr, "[apm] descartada 0x%08X: GPU %.1f MHz, memoria %.1f MHz (%s)\n", c.config,
                 double(gpu1) / 1e6, double(emc1) / 1e6,
                 !gpu_subio ? "la GPU no sube lo pedido" : "MUEVE LA MEMORIA");
  }

  /*
   * None worked. The memory is not lowered by hand: that was tried eight times with clkrst, all
   * eight were accepted and all eight were reverted by the system in under a second. pcv reimposes
   * the EMC of the active apm configuration, and clkrst cannot fight that.
   *
   * So the console is left as it was, and the log says exactly what trade is available and how to
   * take it (nfsmw_switch_ram_1600).
   */
  if (hay_original && R_SUCCEEDED(apmSetPerformanceConfiguration(modo, config_original))) {
    u32 gpu2 = 0, emc2 = 0;
    MemoriaQuietaDurante(emc0, gpu2, emc2);
    std::fprintf(stderr,
                 "[apm] este firmware NO tiene ninguna configuracion de %d MHz que deje la memoria "
                 "en %.1f MHz; se vuelve a la original 0x%08X (GPU %.1f MHz, memoria %.1f MHz).\n"
                 "[apm] SI PREFIERES LA GPU ALTA AUNQUE LA RAM SUBA A 1600: pon "
                 "nfsmw_switch_ram_1600 = true en nfsmw.toml\n",
                 mhz, double(emc0) / 1e6, config_original, double(gpu2) / 1e6, double(emc2) / 1e6);
  } else {
    std::fprintf(stderr, "[apm] ninguna configuracion de %d MHz sirvio y NO se pudo restaurar la "
                         "original: revisa los relojes en el overlay\n",
                 mhz);
  }
}

extern "C" void RexSwitchApmVigilar(void) {
  const u32 emc0 = g_emc_original.load(std::memory_order_relaxed);
  if (emc0 == 0 || g_rendido_emc.load(std::memory_order_relaxed)) {
    return;  // nothing changed, or we already know this firmware does not allow it
  }

  const int tic = g_tics_emc.fetch_add(1, std::memory_order_relaxed);
  if (tic >= kTicsSeguidos && (tic % kTicsEspaciados) != 0) {
    return;
  }

  u32 gpu = 0, emc = 0;
  if (!LeerRelojes(gpu, emc) || emc <= emc0 + kMargenHz) {
    return;  // where it should be
  }

  /* It moved: the configuration raised it after we checked. */
  const bool bajada = FijarMemoria(emc0);
  const int n = g_correcciones_emc.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!g_aviso_emc.exchange(true)) {
    std::fprintf(stderr,
                 "[apm] la memoria habia subido sola a %.1f MHz (t=%d s): se baja a %.1f con clkrst "
                 "(%s)\n",
                 double(emc) / 1e6, tic, double(emc0) / 1e6, bajada ? "aceptado" : "RECHAZADO");
  }
  if (!bajada || n >= kMaxCorreccionesEmc) {
    g_rendido_emc.store(true, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[apm] se deja de insistir con la memoria tras %d intento(s): este firmware la "
                 "reimpone. La GPU se queda en lo pedido; la RAM, en lo que mande el sistema\n",
                 n);
  }
}

#else   // !REX_PLATFORM_SWITCH
extern "C" void RexSwitchApmPedirGpuMhz(int) {}
extern "C" void RexSwitchApmPermitirRam1600(int) {}
extern "C" void RexSwitchApmAplicar(void) {}
extern "C" void RexSwitchApmVigilar(void) {}
#endif  // REX_PLATFORM_SWITCH
