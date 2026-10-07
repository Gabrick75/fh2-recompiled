/*
 * Docked clocks when Reverse-NX says docked.
 *
 * Reverse-NX only lies to the game. Nobody lies to the sysmodule that sets the clocks (sys-clk and its
 * forks, including Horizon OC's sys-clk-plus): to choose between the handheld and docked columns of the
 * profile it asks the hardware (`Board::GetProfile()` -> `apmExtGetPerformanceMode()`), so with the
 * console in hand it always takes the handheld column, whether the profile is the game's or the global
 * one.
 *
 * sys-clk has a command to learn about Reverse-NX (`SysClkIpcCmd_SetReverseNXRTMode`), but in Horizon OC
 * it is empty: `IpcService::SetReverseNXRTMode(mode) { return 0; }`, and the call to
 * `rnxSync->ToggleSync(...)` is commented out in its clock_manager. That is why nothing happens however
 * much the mode is changed.
 *
 * What does work in Horizon OC is the manual override, which is what its own overlay uses when the
 * sliders are moved, and which takes precedence over everything in its loop:
 *
 *     targetHz = overrideFreqs[module];                                       // 1st: what sys:clk is told
 *     if (!targetHz) targetHz = GetAutoClockHz(applicationId, ..., profile);  // 2nd: this game's profile
 *     if (!targetHz) targetHz = GetAutoClockHz(GLOBAL_PROFILE_ID, ..., ...);  // 3rd: the global one
 *
 * So here the sysmodule itself is asked for the numbers configured for the docked column (command 5,
 * GetProfiles, first the game's and otherwise the global profile's, the same way its loop does) and
 * they are sent with command 8 (SetOverride) while Reverse-NX says docked. On returning to handheld
 * they are released.
 *
 * No frequency is invented: if nothing is set in that column, nothing is touched.
 *
 * Careful: the override does not expire. It is released on returning to handheld, when the switch is
 * turned off and when the game exits cleanly (destructor), but if the game closes abnormally it stays
 * set until it is touched in the Horizon OC overlay or the console is restarted.
 *
 * The expensive lesson: `smGetService` for a service that is not registered does not return an error,
 * it leaves the thread waiting forever for someone to register it. That hung the profiler thread as
 * soon as docked mode was selected (Horizon OC does not register "sys:clk"), and with that thread the
 * Reverse-NX heartbeat stopped too (hence the mode never returned to Handheld) and, behind that sm
 * request, the audio. Now sm command 65100 (AtmosphereHasService) is asked first, which answers yes or
 * no and never blocks.
 *
 * References: Horizon-OC/sys-clk-plus (common/include/sysclk/ipc.h, sysmodule/src/clock_manager.cpp and
 * config.cpp); Atmosphere-NX/Atmosphere (sm_user_interface.hpp) for command 65100.
 */

#include "rex/ui/switch_sysclk.h"

#if REX_PLATFORM_SWITCH

#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstring>

/* This object is compiled separately (rexglue_switch_startup) and has no fmt: warnings go to stderr. */

namespace rex::ui::switch_sysclk {
namespace {

constexpr unsigned kModulos = 3;   // CPU, GPU, MEM
constexpr unsigned kPerfiles = 5;  // portatil, +cargando, +cargando USB, +cargador oficial, sobremesa
constexpr unsigned kPerfilSobremesa = 4;
constexpr unsigned kPerfilPortatil = 0;

/*
 * Stock handheld clocks, used when nothing is set in that column. They are the factory ones, without a
 * drop of overclock: CPU the same in both modes (not touched), GPU 307.2 and memory 1331.2. With the
 * console in the dock the sysmodule takes the docked column and stays at 768/1600, so for Reverse-NX's
 * "Fake Handheld" to mean anything they have to be forced.
 */
constexpr uint32_t kPortatilDeSerieHz[kModulos] = {0u, 307200000u, 1331200000u};
constexpr uint64_t kPerfilGlobal = 0xA111111111111111ull;  // GLOBAL_PROFILE_ID de sys-clk

constexpr uint32_t kOrdenPerfiles = 5;   // SysClkIpcCmd_GetProfiles
constexpr uint32_t kOrdenOverride = 8;   // SysClkIpcCmd_SetOverride

/* SysClkTitleProfileList: the configured MHz, per profile and module. */
struct ListaPerfiles {
  uint32_t mhz[kPerfiles][kModulos];
};

/* SysClkIpc_SetOverride_Args. */
struct ArgsOverride {
  uint32_t modulo;
  uint32_t hz;
};

void Aviso(const char* texto) { std::fprintf(stderr, "%s\n", texto); }
void AvisoNum(const char* texto, long long numero) {
  std::fprintf(stderr, "%s 0x%llX (%lld)\n", texto, (unsigned long long)numero, numero);
}

Service g_servicio{};
bool g_abierto = false;
int g_intentos = 0;
bool g_aplicado = false;             // we are forcing the clocks of a mode that is not the real one
bool g_aplicado_sobremesa = false;   // and in which direction (true docked, false handheld)
bool g_puesto[kModulos] = {false, false, false};  // which modules WE have changed
std::atomic<int> g_habilitado{0};  // off by default: this raises the clocks, which must be requested explicitly

Result PedirPerfiles(uint64_t tid, ListaPerfiles* salida) {
  return serviceDispatchIn(&g_servicio, kOrdenPerfiles, tid,
                           .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                           .buffers = {{salida, sizeof(*salida)}});
}

/*
 * sys-clk command 0: GetApiVersion. It checks that on the other side there really is a
 * sysmodule with this interface and not something else with a similar name.
 */
Result PedirVersionApi(uint32_t* salida) {
  return serviceDispatchOut(&g_servicio, 0, *salida);
}

/*
 * sys-clk command 1: GetVersionString. Read-only. It identifies the exact fork and version, which
 * is the only way to know which commands its API really has without sending it anything blindly.
 */
Result PedirVersionTexto(char* salida, size_t bytes) {
  return serviceDispatch(&g_servicio, 1,
                         .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                         .buffers = {{salida, bytes}});
}

/*
 * sys-clk command 2: GetCurrentContext. Read-only.
 *
 * It returns a structure as raw data, and its size changes between forks. The size for hoc:clk is
 * unknown, so several are tried: if the size does not match, the dispatch fails and that is all
 * (nothing is written anywhere). For the one that works, the bytes are dumped in hexadecimal.
 * Comparing that dump in Fake Handheld and in Fake Docked shows which field is the profile, without
 * having to guess the structure.
 */
/*
 * The structure size comes from the type, hence one template per size. If it does not match what
 * the sysmodule returns, the dispatch fails and nothing else happens.
 */
template <unsigned N>
Result PedirContextoDe(unsigned char* salida) {
  struct Bloque {
    unsigned char b[N];
  };
  Bloque tmp{};
  const Result rc = serviceDispatchOut(&g_servicio, 2, tmp);
  if (R_SUCCEEDED(rc)) {
    std::memcpy(salida, tmp.b, N);
  }
  return rc;
}

void Volcar(const unsigned char* buf, unsigned tam) {
  char linea[340];
  int n = std::snprintf(linea, sizeof(linea), "[relojes] contexto (%u bytes):", tam);
  for (unsigned i = 0; i < tam && n > 0 && unsigned(n) + 4 < sizeof(linea); ++i) {
    n += std::snprintf(linea + n, sizeof(linea) - unsigned(n), " %02X", buf[i]);
  }
  Aviso(linea);
}

void VolcarContexto() {
  unsigned char buf[80];
  std::memset(buf, 0, sizeof(buf));
  if (R_SUCCEEDED(PedirContextoDe<28>(buf))) { Volcar(buf, 28); return; }
  if (R_SUCCEEDED(PedirContextoDe<32>(buf))) { Volcar(buf, 32); return; }
  if (R_SUCCEEDED(PedirContextoDe<36>(buf))) { Volcar(buf, 36); return; }
  if (R_SUCCEEDED(PedirContextoDe<40>(buf))) { Volcar(buf, 40); return; }
  if (R_SUCCEEDED(PedirContextoDe<44>(buf))) { Volcar(buf, 44); return; }
  if (R_SUCCEEDED(PedirContextoDe<48>(buf))) { Volcar(buf, 48); return; }
  if (R_SUCCEEDED(PedirContextoDe<52>(buf))) { Volcar(buf, 52); return; }
  if (R_SUCCEEDED(PedirContextoDe<56>(buf))) { Volcar(buf, 56); return; }
  if (R_SUCCEEDED(PedirContextoDe<64>(buf))) { Volcar(buf, 64); return; }
  if (R_SUCCEEDED(PedirContextoDe<72>(buf))) { Volcar(buf, 72); return; }
  Aviso("[relojes] GetCurrentContext no cuela con ninguno de los tamanos probados");
}

Result PonerOverride(uint32_t modulo, uint32_t hz) {
  const ArgsOverride args{modulo, hz};
  return serviceDispatchIn(&g_servicio, kOrdenOverride, args);
}

/*
 * Is that service registered? sm command 65100 (AtmosphereHasService). It answers yes or no; it never
 * waits. Without this, requesting a service that does not exist hangs the thread forever. On 12.0.0+
 * sm speaks TIPC, and CMIF on earlier versions.
 */
bool HayServicio(const char* nombre) {
  const SmServiceName codificado = smEncodeName(nombre);
  bool hay = false;
  if (hosversionAtLeast(12, 0, 0)) {
    TipcService* sm = smGetServiceSessionTipc();
    if (!sm || R_FAILED(tipcDispatchInOut(sm, 65100, codificado, hay))) {
      return false;
    }
  } else {
    Service* sm = smGetServiceSession();
    if (!sm || R_FAILED(serviceDispatchInOut(sm, 65100, codificado, hay))) {
      return false;
    }
  }
  return hay;
}

bool Abrir() {
  if (g_abierto) {
    return true;
  }
  if (g_intentos >= 5) {
    return false;  // not installed; do not keep insisting forever
  }
  ++g_intentos;
  /*
   * Not only "sys:clk".
   *
   * Horizon OC 2.4.2 does not register that name, and giving up there with a warning meant the clocks
   * never followed Reverse-NX on such a console. sys-clk-plus does use "sys:clk" with the same
   * commands (5 GetProfiles, 8 SetOverride), so the interface is the usual one; what changes between
   * forks is the name. The known ones are tried and the log says which exist, which is the only way to
   * find out the name on a console that is not at hand.
   *
   * HayServicio answers yes or no and never waits, so asking about several is free.
   */
  static const char* const kNombres[] = {"sys:clk",  "hoc:clk", "hocclk",
                                         "sysclk",   "clk:sys", "sys:oc"};
  const char* elegido = nullptr;
  char hallados[160];
  hallados[0] = '\0';
  for (const char* nombre : kNombres) {
    if (!HayServicio(nombre)) {
      continue;
    }
    if (hallados[0]) {
      std::strncat(hallados, ", ", sizeof(hallados) - std::strlen(hallados) - 1);
    }
    std::strncat(hallados, nombre, sizeof(hallados) - std::strlen(hallados) - 1);
    if (!elegido) {
      elegido = nombre;
    }
  }
  if (!elegido) {
    if (g_intentos == 1) {
      Aviso("[relojes] no encuentro ningun sysmodule de relojes conocido (probados: sys:clk, hoc:clk, "
            "hocclk, sysclk, clk:sys, sys:oc): no hay a quien pedirselo");
    }
    g_intentos = 5;  // no volver a preguntar
    return false;
  }
  const Result rc = smGetService(&g_servicio, elegido);
  if (R_FAILED(rc)) {
    if (g_intentos == 1) {
      std::fprintf(stderr, "[relojes] hay '%s' pero no se pudo abrir. Error 0x%X\n", elegido,
                   (unsigned)rc);
    }
    return false;
  }
  // Make sure it really speaks our interface before sending it clock commands.
  uint32_t version = 0;
  if (R_FAILED(PedirVersionApi(&version))) {
    std::fprintf(stderr,
                 "[relojes] '%s' abierto pero no contesta a GetApiVersion: no es la interfaz de "
                 "sys-clk, no se toca nada (servicios hallados: %s)\n",
                 elegido, hallados);
    serviceClose(&g_servicio);
    g_intentos = 5;
    return false;
  }
  char version_texto[64];
  std::memset(version_texto, 0, sizeof(version_texto));
  if (R_SUCCEEDED(PedirVersionTexto(version_texto, sizeof(version_texto)))) {
    version_texto[sizeof(version_texto) - 1] = 0;
    char linea[160];
    std::snprintf(linea, sizeof(linea), "[relojes] version del sysmodule: %s", version_texto);
    Aviso(linea);
  } else {
    Aviso("[relojes] el sysmodule no contesta a GetVersionString (orden 1)");
  }
  VolcarContexto();
  std::fprintf(stderr, "[relojes] sysmodule '%s', API %u (servicios hallados: %s)\n", elegido,
               (unsigned)version, hallados);
  g_abierto = true;
  Aviso("[relojes] listo: los relojes pueden seguir a Reverse-NX");
  return true;
}

/*
 * The MHz of the docked column: first this game's and, if there are none, the global profile's. It is
 * the same order the sysmodule follows.
 */
void LeerPerfil(unsigned perfil, uint32_t salida[kModulos]) {
  std::memset(salida, 0, sizeof(uint32_t) * kModulos);

  uint64_t tid = 0;
  svcGetInfo(&tid, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);

  ListaPerfiles lista{};
  if (tid && R_SUCCEEDED(PedirPerfiles(tid, &lista))) {
    for (unsigned m = 0; m < kModulos; ++m) {
      salida[m] = lista.mhz[perfil][m];
    }
  }
  bool falta = false;
  for (unsigned m = 0; m < kModulos; ++m) {
    if (!salida[m]) {
      falta = true;
    }
  }
  if (!falta) {
    return;
  }
  ListaPerfiles global{};
  if (R_SUCCEEDED(PedirPerfiles(kPerfilGlobal, &global))) {
    for (unsigned m = 0; m < kModulos; ++m) {
      if (!salida[m]) {
        salida[m] = global.mhz[perfil][m];
      }
    }
  }
}

void Aplicar(bool a_sobremesa) {
  // The state is marked whatever happens: if nothing is configured, there is no need to retry every
  // second or to repeat the warning.
  g_aplicado = true;
  uint32_t mhz[kModulos] = {0, 0, 0};
  LeerPerfil(a_sobremesa ? kPerfilSobremesa : kPerfilPortatil, mhz);
  uint32_t hz[kModulos] = {0, 0, 0};
  for (unsigned m = 0; m < kModulos; ++m) {
    hz[m] = mhz[m] * 1000000u;
  }
  if (!a_sobremesa) {
    // Going down to handheld has to do something: if that column is empty the stock clocks are used,
    // which is what "Fake Handheld" is expected to mean.
    for (unsigned m = 0; m < kModulos; ++m) {
      if (!hz[m]) {
        hz[m] = kPortatilDeSerieHz[m];
      }
    }
  }
  if (!hz[0] && !hz[1] && !hz[2]) {
    Aviso("[relojes] no tienes nada puesto en esa columna: no se toca nada");
    return;
  }
  for (unsigned m = 0; m < kModulos; ++m) {
    if (hz[m] && R_SUCCEEDED(PonerOverride(m, hz[m]))) {
      g_puesto[m] = true;
    }
  }
  std::fprintf(stderr,
               "[relojes] %s por Reverse-NX: CPU %u kHz, GPU %u kHz, memoria %u kHz (0 = como estaba)\n",
               a_sobremesa ? "sobremesa" : "portatil", hz[0] / 1000u, hz[1] / 1000u, hz[2] / 1000u);
}

void Soltar() {
  g_aplicado = false;
  bool alguno = false;
  for (unsigned m = 0; m < kModulos; ++m) {
    // Only what we set is released: a clock forced by hand from the overlay is not cleared.
    if (g_puesto[m]) {
      PonerOverride(m, 0);
      g_puesto[m] = false;
      alguno = true;
    }
  }
  if (alguno) {
    Aviso("[relojes] soltados: los vuelve a poner el sysmodule");
  }
}

/* In case the game exits cleanly: never leave the docked clocks set in handheld mode. */
__attribute__((destructor)) void AlSalir() {
  if (g_abierto) {
    Soltar();
  }
}

}  // namespace

void SeguirModo(bool sobremesa_efectivo, bool sobremesa_real) {
  // Both directions. Faking docked mode with the console in hand is not enough: the opposite case
  // (console in the dock and Reverse-NX on "Fake Handheld") left everything untouched and the
  // sysmodule kept the docked column, that is, GPU 768 MHz and memory 1600. It acts whenever the
  // effective mode and the real one differ.
  const bool queremos =
      g_habilitado.load(std::memory_order_relaxed) != 0 && sobremesa_efectivo != sobremesa_real;
  if (queremos == g_aplicado && (!queremos || sobremesa_efectivo == g_aplicado_sobremesa)) {
    return;
  }
  if (!Abrir()) {
    return;
  }
  if (queremos) {
    if (g_aplicado && sobremesa_efectivo != g_aplicado_sobremesa) {
      Soltar();  // direction change: remove the old one, then apply the new one
    }
    g_aplicado_sobremesa = sobremesa_efectivo;
    Aplicar(sobremesa_efectivo);
  } else {
    Soltar();
  }
}

void Habilitar(bool habilitado) {
  g_habilitado.store(habilitado ? 1 : 0, std::memory_order_relaxed);
  if (!habilitado && g_aplicado) {
    Soltar();
  }
}

}  // namespace rex::ui::switch_sysclk

extern "C" void RexSwitchRelojesReverseHabilitar(int habilitado) {
  rex::ui::switch_sysclk::Habilitar(habilitado != 0);
}

#endif  // REX_PLATFORM_SWITCH
