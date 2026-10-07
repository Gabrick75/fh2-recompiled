/*
 * Publish FPS and resolution where the console monitor reads them, and obey Reverse-NX.
 *
 * The FPS counter of those overlays (Status Monitor Overlay and its forks, such as "Horizon OC
 * Monitor") is not computed by them: it is read from shared memory published by SaltyNX. That
 * memory is filled in by NX-FPS, which hooks the game's present call (nvnQueuePresentTexture,
 * eglSwapBuffers or vkQueuePresentKHR). Our game goes through none of the three (NVK is linked
 * inside the NRO and talks to nvdrv), so there is nothing to hook and the field stays empty.
 *
 * Here we write the block ourselves. The format is SaltyNX's (struct NxFpsSharedBlock, 174 packed
 * bytes, magic 0x465053), and the overlay finds it by scanning the shared memory 4 bytes at a time.
 *
 * The Reverse-NX block (magic "NXRT") is read as well; that one does publish its state even though
 * it cannot hook us: this way the game can follow the handheld/docked mode chosen in its overlay
 * without putting the console in the dock.
 *
 * If SaltyNX is not installed, the port does not exist and nothing is done here.
 *
 * References: masagrator/SaltyNX (saltysd_core), masagrator/Status-Monitor-Overlay
 * (source/Utils.hpp) and masagrator/ReverseNX-RT (Overlay/include/SaltyNX.h).
 */

#include "rex/ui/switch_saltynx.h"

#if REX_PLATFORM_SWITCH

#include <switch.h>

#include <atomic>
#include <cstring>

#include <cstdio>

/*
 * This object is compiled separately (rexglue_switch_startup) and has no fmt, so no cvars and no
 * REXLOG: warnings go to stderr, which on the console ends up in rex_stderr.log.
 */

namespace rex::ui::switch_saltynx {
namespace {

constexpr uint32_t kMagicFps = 0x465053;          // «SPF»: bloque de NX-FPS
constexpr uint32_t kMagicReverseNx = 0x5452584E;  // «NXRT» en little-endian
constexpr size_t kTamanoCompartido = 0x1000;      // SaltyNX maps one page

/* Resolution calls as the overlay reads them: width, height and how many times. */
struct ResolucionLlamadas {
  uint16_t ancho;
  uint16_t alto;
  uint16_t llamadas;
} __attribute__((packed));

/* SaltyNX's struct NxFpsSharedBlock. The overlay checks that it is 174 bytes. */
struct BloqueFps {
  uint32_t magic;
  uint8_t fps;
  float fps_media;
  bool plugin_activo;
  uint8_t fps_bloqueados;
  uint8_t modo_fps;
  uint8_t zero_sync;
  uint8_t parche_aplicado;
  uint8_t api;
  uint32_t ticks[10];
  uint8_t buffers;
  uint8_t buffers_puestos;
  uint8_t buffers_activos;
  uint8_t buffers_activos_puestos;
  uint8_t display_sync;
  ResolucionLlamadas render[8];
  ResolucionLlamadas viewport[8];
  bool forzar_refresco_original;
  bool no_forzar_60_en_base;
  bool forzar_suspension;
  uint8_t refresco_actual;
  float lectura_por_segundo;
  uint8_t fps_bloqueados_base;
  uint64_t numero_fotograma;
  int8_t buffers_esperados;
} __attribute__((packed));

static_assert(sizeof(BloqueFps) == 174, "el overlay espera 174 bytes");

/* struct Shared de ReverseNX-RT (9 bytes). */
struct BloqueReverseNx {
  uint32_t magic;
  bool en_base;
  bool por_defecto;
  bool plugin_activo;
  uint8_t resoluciones;
  bool uso_ddr;
} __attribute__((packed));

static_assert(sizeof(BloqueReverseNx) == 9, "ReverseNX-RT usa 9 bytes");

/* API declared by the block: 0 unknown, 1 NVN, 2 GL, 3 Vulkan. */
constexpr uint8_t kApiVulkan = 3;

/* Warnings to stderr, which on the console ends up in rex_stderr.log. */
void Aviso(const char* texto) { std::fprintf(stderr, "%s\n", texto); }
void AvisoNum(const char* texto, long long numero) {
  std::fprintf(stderr, "%s 0x%llX (%lld)\n", texto, (unsigned long long)numero, numero);
}
void AvisoDos(const char* texto, long long a, long long b) {
  std::fprintf(stderr, "%s %lld / %lld\n", texto, a, b);
}

SharedMemory g_memoria{};
bool g_mapeada = false;
uint8_t* g_base = nullptr;   // start of the shared area, whether from IPC or from the scan
size_t g_bytes = 0;
/*
 * Written by the profiler thread (Iniciar/Actualizar) and read by the ring thread on every present
 * (Latir), hence atomic.
 */
std::atomic<BloqueFps*> g_fps{nullptr};
uint64_t g_tick_previo = 0;   // ring thread only
unsigned g_tick_pos = 0;      // idem
uint64_t g_fotogramas = 0;    // idem
std::atomic<BloqueReverseNx*> g_reverse{nullptr};
std::atomic<bool> g_reverse_en_base{false};
std::atomic<bool> g_reverse_activo{false};
/* Turned on by the game (cvar nfsmw_switch_saltynx). On by default. */
std::atomic<int> g_habilitado{1};
/*
 * Connection attempts left. The sysmodule may take longer than us to start, so if it is not there at
 * first, it is retried once per second during the first minute.
 */
int g_intentos = 0;  // how many attempts so far

/*
 * The last values published. If the block appears late (because the overlay was opened later,
 * SaltyNX was slow to hand out the memory, or the first allocation did not fit), it has to be seeded
 * right away with these values: otherwise the overlay shows no FPS until the next second and no
 * resolution until the next present, and during loading there may be neither for several seconds.
 * Only the profiler thread touches them (Actualizar / Iniciar).
 */
uint8_t g_ultimo_fps = 0;
float g_ultima_media = 0.0f;
uint16_t g_ultimo_ancho = 0;
uint16_t g_ultimo_alto = 0;
uint64_t g_ultimos_fotogramas = 0;

/*
 * Tick of the first attempt. The profiler thread calls Iniciar() right at startup (it does not wait
 * for the 8 s of kStartDelayNs), so it serves as the "startup" reference for the success message.
 */
uint64_t g_tick_primer_intento = 0;
/* warnings issued only once; repeating them on every retry filled the log. */
bool g_avisado_publicado = false;
bool g_avisado_conexion = false;
bool g_avisado_bloque = false;
bool g_avisado_sin_sitio = false;
bool g_avisado_reverse = false;
bool g_avisado_sin_memoria = false;

/*
 * --- SaltySD IPC. Modern libnx no longer ships the old ipc.h API, but the service speaks plain
 * CMIF, so serviceDispatch works: the header carries the same SFCI and the body the same fields. ---
 */

Result ReservarMemoria(Service* s, uint64_t tamano, uint64_t* desplazamiento) {
  return serviceDispatchInOut(s, 6, tamano, *desplazamiento, .in_send_pid = true);
}

Result PedirManejador(Service* s, Handle* salida) {
  return serviceDispatch(s, 7, .in_send_pid = true,
                         .out_handle_attrs = {SfOutHandleAttr_HipcCopy}, .out_handles = salida);
}

Result Terminar(Service* s) {
  const uint64_t cero = 0;
  return serviceDispatchIn(s, 0, cero, .in_send_pid = true);
}

/* Looks for a magic by scanning the page 4 bytes at a time, which is how the overlay does it. */
void* BuscarMarcaEn(uint8_t* base, size_t bytes, uint32_t marca) {
  if (!base) {
    return nullptr;
  }
  for (size_t offset = 0; offset + sizeof(uint32_t) <= bytes; offset += 4) {
    uint32_t leido = 0;
    std::memcpy(&leido, base + offset, sizeof(leido));
    if (leido == marca) {
      return base + offset;
    }
  }
  return nullptr;
}

void* BuscarMarca(uint32_t marca) { return BuscarMarcaEn(g_base, g_bytes, marca); }

/*
 * The block is only valid while it keeps its magic. The SaltySD page is shared by several clients
 * and its allocation is not reset when we start, so a good pointer can go bad. Checking it is a
 * 4-byte read, and it is done on every use.
 */
bool BloqueValido(const BloqueFps* bloque) { return bloque != nullptr && bloque->magic == kMagicFps; }

/*
 * Writes into the block, right away, everything the overlay needs to show its two rows (FPS and
 * RES): the alive mark, the API, the frames per second and the resolution. The overlay shows neither
 * row until the corresponding field has something, so seeding it as soon as the block exists is
 * what makes the data appear without restarting the game.
 */
void SembrarBloque(BloqueFps* bloque) {
  bloque->plugin_activo = true;  // the overlay sets it to false to check that we are still alive
  bloque->api = kApiVulkan;
  bloque->fps = g_ultimo_fps;
  bloque->fps_media = g_ultima_media;
  bloque->numero_fotograma = g_ultimos_fotogramas;
  if (g_ultimo_ancho && g_ultimo_alto) {
    // `llamadas` cannot be 0xFFFF: that is the mark the overlay uses to ask whether we know the resolution.
    const uint16_t cuantas = g_ultimo_fps ? uint16_t(g_ultimo_fps) : uint16_t(1);
    const ResolucionLlamadas r = {g_ultimo_ancho, g_ultimo_alto, cuantas};
    bloque->render[0] = r;
    bloque->viewport[0] = r;
  }
  // The overlay computes its average from ticks[], not from fps_media: with the array at zero it shows
  // "inf". A freshly created block has it at zero, so if a rate has already been measured it is
  // filled in by hand. Latir corrects it with real times as soon as there are two presents.
  if (g_ultimo_fps != 0 && bloque->ticks[0] == 0) {
    // By index, not by reference: the block is packed and ticks[] sits at an odd offset; taking the
    // address of an element would give an unaligned pointer (-Waddress-of-packed-member).
    const uint32_t por_fotograma = uint32_t(armGetSystemTickFreq() / g_ultimo_fps);
    for (unsigned i = 0; i < 10; ++i) {
      bloque->ticks[i] = por_fotograma;
    }
  }
}

/*
 * A single log line when publishing succeeds, with the seconds since startup. Without it there is
 * no way to know whether the overlay is late because of us or because of it.
 */
void AvisarPublicado(const BloqueFps* bloque) {
  if (g_avisado_publicado) {
    return;
  }
  g_avisado_publicado = true;
  const uint64_t freq = armGetSystemTickFreq();
  const double segundos =
      freq ? double(armGetSystemTick() - g_tick_primer_intento) / double(freq) : 0.0;
  const unsigned desplazamiento =
      g_base ? unsigned(reinterpret_cast<const uint8_t*>(bloque) - static_cast<const uint8_t*>(g_base)) : 0u;
  std::fprintf(stderr,
               "[saltynx] PUBLICADO a los %.2f s del arranque: FPS=%u y RES=%ux%u en el desplazamiento 0x%X\n",
               segundos, unsigned(g_ultimo_fps), unsigned(g_ultimo_ancho), unsigned(g_ultimo_alto),
               desplazamiento);
}

/*
 * With the page already mapped, looks for both magics and keeps them. It is cheap (1024 comparisons
 * of 4 bytes on memory that is already there) and uses no port session, so it can be repeated every
 * second while the block is missing. Returns true if there is an FPS block.
 */
bool Enganchar() {
  auto* bloque = static_cast<BloqueFps*>(BuscarMarca(kMagicFps));
  if (bloque) {
    g_fps.store(bloque, std::memory_order_release);
    SembrarBloque(bloque);
    AvisarPublicado(bloque);
  }
  if (!g_reverse.load(std::memory_order_acquire)) {
    if (auto* reverse = static_cast<BloqueReverseNx*>(BuscarMarca(kMagicReverseNx))) {
      g_reverse.store(reverse, std::memory_order_release);
    }
  }
  return bloque != nullptr;
}

/*
 * If SaltyNX was injected into this process, its shared memory is already mapped here. The memory
 * map is walked with svcQueryMemory, looking inside the readable shared memory regions.
 */
bool BuscarMemoriaCompartidaPropia() {
  uint64_t direccion = 0;
  for (int regiones = 0; regiones < 4096; ++regiones) {
    MemoryInfo info{};
    u32 paginas = 0;
    if (R_FAILED(svcQueryMemory(&info, &paginas, direccion))) {
      return false;
    }
    if (info.size == 0) {
      return false;
    }
    if (info.type == MemType_SharedMem && (info.perm & Perm_R) != 0 && info.size >= kTamanoCompartido) {
      auto* base = reinterpret_cast<uint8_t*>(uintptr_t(info.addr));
      const size_t bytes = size_t(info.size) < 0x10000 ? size_t(info.size) : 0x10000;
      if (BuscarMarcaEn(base, bytes, kMagicFps) || BuscarMarcaEn(base, bytes, kMagicReverseNx)) {
        g_base = base;
        g_bytes = bytes;
        return true;
      }
    }
    const uint64_t siguiente = info.addr + info.size;
    if (siguiente <= direccion) {
      return false;
    }
    direccion = siguiente;
  }
  return false;
}

/*
 * A connection error can come from two very different things that look much alike:
 *   - the single session of the SaltyNX port is busy (its limit, fixed by retrying), or
 *   - we are the ones out of sessions (0x10801 is "resource exhausted", and the kernel also returns
 *     it when the process cannot reserve another session).
 * They are told apart with a control: connecting to "sm:", which always exists and accepts many
 * sessions. If "sm:" fails too, the limit is ours; and the used sessions and the process cap are
 * printed right there.
 */
void Diagnostico(Result rc_saltysd) {
  if (rc_saltysd == 0) {
    Aviso("[saltynx] ni se ha intentado: el proceso no tiene sitio para otra sesion de puerto");
  } else {
    AvisoNum("[saltynx] no conecta con los puertos de SaltyNX. Ultimo error", rc_saltysd);
  }

  // Our own title ID: SaltyNX rejects those above 0x01FFFFFFFFFFFFFF ("is a homebrew application"),
  // which is exactly the range forwarders fall in. Knowing it saves a question.
  u64 titulo = 0;
  if (R_SUCCEEDED(svcGetInfo(&titulo, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0))) {
    std::fprintf(stderr, "[saltynx] nuestro TID: %016llX (SaltyNX admite <= 01FFFFFFFFFFFFFF y sin 0x1F00)\n",
                 (unsigned long long)titulo);
  }

  // Each port separately: "does not exist" (0xF201, not installed) is not the same as "resource exhausted".
  static const char* const kDosPuertos[2] = {"InjectServ", "SaltySD"};
  for (const char* p : kDosPuertos) {
    Handle h = INVALID_HANDLE;
    const Result rc = svcConnectToNamedPort(&h, p);
    if (R_SUCCEEDED(rc)) {
      svcCloseHandle(h);
      Aviso(p[0] == 'I' ? "[saltynx] puerto InjectServ: SI conecta" : "[saltynx] puerto SaltySD: SI conecta");
    } else {
      AvisoNum(p[0] == 'I' ? "[saltynx] puerto InjectServ: error" : "[saltynx] puerto SaltySD: error", rc);
    }
  }

  Handle control = INVALID_HANDLE;
  const Result rc_sm = svcConnectToNamedPort(&control, "sm:");
  if (R_SUCCEEDED(rc_sm)) {
    svcCloseHandle(control);
    Aviso("[saltynx] control: 'sm:' SI da sesion nueva, asi que el limite es del puerto de SaltyNX");
  } else {
    AvisoNum("[saltynx] control: 'sm:' tampoco da sesion, asi que el limite es NUESTRO. Error", rc_sm);
  }

  u64 bruto = 0;
  Result rc_lim = svcGetInfo(&bruto, InfoType_ResourceLimit, INVALID_HANDLE, 0);
  if (R_FAILED(rc_lim)) {
    rc_lim = svcGetInfo(&bruto, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0);
  }
  if (R_FAILED(rc_lim)) {
    AvisoNum("[saltynx] no se pueden leer los limites del proceso. Error", rc_lim);
    return;
  }
  const Handle limite = static_cast<Handle>(bruto);
  struct Recurso {
    const char* nombre;
    LimitableResource cual;
  };
  const Recurso kRecursos[] = {
      {"[saltynx] sesiones usadas / tope:", LimitableResource_Sessions},
      {"[saltynx] eventos usados / tope:", LimitableResource_Events},
      {"[saltynx] hilos usados / tope:", LimitableResource_Threads},
      {"[saltynx] memorias transferibles usadas / tope:", LimitableResource_TransferMemories},
  };
  for (const Recurso& r : kRecursos) {
    s64 ahora = 0;
    s64 tope = 0;
    if (R_SUCCEEDED(svcGetResourceLimitCurrentValue(&ahora, limite, r.cual)) &&
        R_SUCCEEDED(svcGetResourceLimitLimitValue(&tope, limite, r.cual))) {
      AvisoDos(r.nombre, (long long)ahora, (long long)tope);
    }
  }
  svcCloseHandle(limite);
}

/*
 * --- Making room for a session. ---------------------------------------------------------------------
 * Measured on the console: this process (forwarder + hbloader) has a resource limit with "sesiones
 * usadas / tope: 1 / 1". A single port session, and libnx already uses it for "sm:". That is why
 * both SaltyNX ports and also the "sm:" control failed, all three with 0x10801 (resource
 * exhausted). It is not SaltyNX's fault.
 *
 * Two ways to make room, in this order:
 *   1. Raise the cap of our own resource limit. The kernel requires no privilege for that: only that
 *      the new cap is not lower than what is already in use. But the call (SVC 0x7E) may not be
 *      allowed in the process, and using a forbidden SVC kills the game, so envIsSyscallHinted is
 *      asked first.
 *   2. If that is not possible, release "sm:" for a moment (smExit) and bring it back afterwards. It
 *      is only needed once: the shared memory handle stays with us even if the session is closed.
 */

Handle AbrirLimiteDeRecursos() {
  u64 bruto = 0;
  if (R_SUCCEEDED(svcGetInfo(&bruto, InfoType_ResourceLimit, INVALID_HANDLE, 0))) {
    return static_cast<Handle>(bruto);
  }
  if (R_SUCCEEDED(svcGetInfo(&bruto, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0))) {
    return static_cast<Handle>(bruto);
  }
  return INVALID_HANDLE;
}

bool HaySitioParaUnaSesion(Handle limite) {
  s64 ahora = 0;
  s64 tope = 0;
  if (R_FAILED(svcGetResourceLimitCurrentValue(&ahora, limite, LimitableResource_Sessions)) ||
      R_FAILED(svcGetResourceLimitLimitValue(&tope, limite, LimitableResource_Sessions))) {
    return true;  // if it cannot be read, try anyway
  }
  return ahora < tope;
}

/*
 * Sets *sm_cerrado to true if "sm:" had to be released (it has to be brought back later).
 * Releasing "sm:" is not tried on every attempt: it is a window of a few milliseconds without the
 * name service and should not be repeated once per second forever. Raising the cap, on the other
 * hand, is permanent and done only once.
 */
bool HacerSitio(bool* sm_cerrado, bool permitir_soltar_sm) {
  *sm_cerrado = false;
  const Handle limite = AbrirLimiteDeRecursos();
  if (limite == INVALID_HANDLE) {
    return true;
  }
  bool sitio = HaySitioParaUnaSesion(limite);

  if (!sitio) {
    if (envIsSyscallHinted(0x7E)) {  // svcSetResourceLimitLimitValue
      s64 tope = 0;
      svcGetResourceLimitLimitValue(&tope, limite, LimitableResource_Sessions);
      const Result rc = svcSetResourceLimitLimitValue(limite, LimitableResource_Sessions,
                                                      static_cast<u64>(tope + 4));
      if (R_SUCCEEDED(rc)) {
        sitio = HaySitioParaUnaSesion(limite);
        AvisoDos("[saltynx] subido el tope de sesiones del proceso:", (long long)tope, (long long)(tope + 4));
      } else {
        AvisoNum("[saltynx] no deja subir el tope de sesiones. Error", rc);
      }
    } else {
      Aviso("[saltynx] el cargador no permite svcSetResourceLimitLimitValue (SVC 0x7E)");
    }
  }

  if (!sitio && permitir_soltar_sm) {
    smExit();  // libnx reference-counts it; if it really closes, there is room
    if (HaySitioParaUnaSesion(limite)) {
      *sm_cerrado = true;
      sitio = true;
      Aviso("[saltynx] soltado 'sm:' un momento para tener sitio");
    } else {
      smInitialize();  // it did not close: restore the count and leave it as it was
      Aviso("[saltynx] ni soltando 'sm:' hay sitio para una sesion");
    }
  }

  svcCloseHandle(limite);
  return sitio;
}

/* Brings "sm:" back on exit, whatever happens. */
struct DevolverSm {
  bool activo = false;
  ~DevolverSm() {
    if (activo) {
      smInitialize();
    }
  }
};

}  // namespace

void Iniciar() {
  if (!g_habilitado.load(std::memory_order_relaxed)) {
    return;
  }
  if (g_tick_primer_intento == 0) {
    g_tick_primer_intento = armGetSystemTick();
  }

  /*
   * The exit condition is not "the page is mapped" but "we have a good block". Otherwise, if
   * mapping succeeded but there was no room for the block (or the magic disappeared because another
   * client rewrote the page), g_mapeada stayed true and this was never retried: the only way out was
   * restarting the game with the overlay already on. The magic is checked and, if missing, the block
   * is attached again.
   */
  BloqueFps* ya = g_fps.load(std::memory_order_acquire);
  if (!BloqueValido(ya)) {
    g_fps.store(nullptr, std::memory_order_release);
  } else if (g_reverse.load(std::memory_order_acquire) != nullptr) {
    return;  // both in place: nothing to do
  }

  // The cheap part first: if the page is already mapped, it is enough to search for the magics again.
  // It uses no sessions and no IPC, so it can be done once per second forever.
  if (g_mapeada && Enganchar()) {
    return;
  }

  // Path 1: IPC. SaltyNX creates two ports and each accepts a single session, so both are tried, with
  // a few retries: the sysmodule may take longer than us to start.
  // First there has to be room: this process only allows one port session and libnx uses it for "sm:".
  DevolverSm devolver;
  // Raising the cap is tried from the very start (it is permanent and bothers nobody). Releasing "sm:"
  // only from the third attempt on, when startup has already opened its services.
  // That window used to close at attempt 15 and never reopen: if the port was busy during that quarter
  // of a minute, the only option was restarting the game. After the window, it is retried once per
  // minute: it is still a pause of a few milliseconds without the name service, but it is no longer
  // abandoned.
  const bool soltar_sm = g_intentos >= 3 && (g_intentos < 15 || (g_intentos % 60) == 0);
  const bool con_sitio = HacerSitio(&devolver.activo, soltar_sm);
  // With "sm:" closed it has to be quick: a single pass. On the first attempt it insists (the
  // sysmodule may be starting up); in the per-second retries two passes are enough, since the overlay
  // also needs the port's only session.
  const int vueltas = devolver.activo ? 1 : (g_intentos == 0 ? 20 : 2);
  bool listo = false;

  /*
   * "SaltySD" first. Both ports accept connections, but the one that serves commands 6 and 7 (shared
   * memory slot and handle) is "SaltySD": it is the one the overlays talk to. "InjectServ" connects
   * and then answers by closing the session (0xF601, ConnectionClosed). An earlier version tried it
   * first, took it as good and spent the whole attempt: 65 seconds looking at the wrong port, once per
   * second, until by chance it was busy and the right one was used. Now, if a port connects but does
   * not give the memory, it is closed and the next one is tried in the same pass.
   */
  static const char* const kPuertos[2] = {"SaltySD", "InjectServ"};

  uint64_t desplazamiento = 0;
  bool reservada = false;
  uint64_t desplazamiento_nx = 0;
  bool reservada_nx = false;
  Result rc_puerto = 0;
  Result rc_manejador = 0;
  const char* nombre = nullptr;

  for (int i = 0; i < vueltas && !listo && con_sitio; ++i) {
    for (const char* candidato : kPuertos) {
      Handle puerto = INVALID_HANDLE;
      rc_puerto = svcConnectToNamedPort(&puerto, candidato);
      if (R_FAILED(rc_puerto)) {
        continue;
      }
      Service servicio{};
      servicio.session = puerto;

      // First the handle and the mapping, and only then allocate what is missing. SaltySD hands out the
      // page with a counter that it only resets when it injects into a game (hijack_bootstrap), and it
      // never injects into us: allocating on every boot would eat the 4 KB in twenty sessions. By
      // checking first whether the block is already there, later boots reuse the same slot.
      // If the page was already mapped by an earlier attempt, the handle is not requested again (that
      // would map the same memory twice): it goes straight to allocating what is missing.
      bool tenemos_pagina = g_mapeada;
      if (!tenemos_pagina) {
        Handle memoria = INVALID_HANDLE;
        rc_manejador = PedirManejador(&servicio, &memoria);
        if (R_SUCCEEDED(rc_manejador)) {
          shmemLoadRemote(&g_memoria, memoria, kTamanoCompartido, Perm_Rw);
          if (R_SUCCEEDED(shmemMap(&g_memoria))) {
            g_base = static_cast<uint8_t*>(shmemGetAddr(&g_memoria));
            g_bytes = kTamanoCompartido;
            g_mapeada = true;
            tenemos_pagina = true;
          }
        }
      }
      if (tenemos_pagina) {
        listo = true;
        nombre = candidato;
        if (!BuscarMarca(kMagicFps)) {
          reservada = R_SUCCEEDED(ReservarMemoria(&servicio, sizeof(BloqueFps), &desplazamiento));
        }
        if (!BuscarMarca(kMagicReverseNx)) {
          reservada_nx = R_SUCCEEDED(ReservarMemoria(&servicio, sizeof(BloqueReverseNx), &desplazamiento_nx));
        }
      }

      Terminar(&servicio);  // command 0: the server closes its side
      // And our end has to be released too. The port accepts a single session
      // (svcManageNamedPort(..., 1)), so leaving the handle open would keep the overlay from ever
      // connecting again. The shared memory handle is already ours and does not depend on the session.
      svcCloseHandle(puerto);
      if (listo) {
        break;
      }
    }
    if (!listo) {
      svcSleepThread(10 * 1000 * 1000);  // 10 ms
    }
  }

  if (devolver.activo) {  // room again: bring "sm:" back without waiting for the end
    smInitialize();
    devolver.activo = false;
  }

  if (nombre && !g_avisado_conexion) {
    g_avisado_conexion = true;  // once; this is retried every second and used to fill the log
    Aviso(nombre[0] == 'S' ? "[saltynx] conectado por SaltySD" : "[saltynx] conectado por InjectServ");
  }
  if (!g_mapeada) {
    if (R_FAILED(rc_manejador) && !g_avisado_sin_memoria) {
      // A port connected but did not serve the memory. The attempt is not lost: the other one has already
      // been tried. Only once: this is retried every second for the whole session and used to fill the log.
      g_avisado_sin_memoria = true;
      AvisoNum("[saltynx] algun puerto conecta pero no da la memoria compartida; error", rc_manejador);
    }
    // Path 2: no free session. If SaltyNX was injected into this process, its shared memory is already mapped here.
    if (!BuscarMemoriaCompartidaPropia()) {
      // It is retried for the whole session, not just one minute. Error 0x10801 (LimitReached) says
      // that the port exists but its only session is busy, so it may be freed later.
      ++g_intentos;
      // The first diagnosis is done on attempt 5, not 1: the first attempt happens at second zero of
      // startup, and it makes no sense to diagnose before having tried releasing "sm:".
      if (g_intentos == 5 || g_intentos == 30) {
        Diagnostico(rc_puerto);
      } else if (g_intentos % 600 == 0) {
        AvisoNum("[saltynx] se sigue reintentando sin exito. Error", rc_puerto);
      }
      return;
    }
    g_mapeada = true;
    Aviso("[saltynx] sin sesion en los puertos, pero su memoria compartida ya estaba mapeada aqui");
  }

  // The FPS block: if SaltyNX already left one in this process it is overwritten (its fields would be
  // 0, since it has nothing to hook); otherwise the slot allocated through IPC is used.
  auto* existente = static_cast<BloqueFps*>(BuscarMarca(kMagicFps));
  if (existente) {
    g_fps.store(existente, std::memory_order_release);
    if (!g_avisado_bloque) {
      g_avisado_bloque = true;
      Aviso("[saltynx] bloque de FPS ya presente: se escribe encima");
    }
  } else if (reservada) {
    auto* bloque = reinterpret_cast<BloqueFps*>(g_base + desplazamiento);
    std::memset(bloque, 0, sizeof(*bloque));
    bloque->magic = kMagicFps;
    g_fps.store(bloque, std::memory_order_release);
    AvisoNum("[saltynx] bloque de FPS creado en el desplazamiento", (long long)desplazamiento);
  } else if (!g_avisado_sin_sitio) {
    g_avisado_sin_sitio = true;
    Aviso("[saltynx] sin sitio para el bloque de FPS; se sigue intentando una vez por segundo");
  }
  if (BloqueFps* bloque = g_fps.load(std::memory_order_acquire)) {
    // FPS and resolution at once, without waiting for the next second or the next present.
    SembrarBloque(bloque);
    AvisarPublicado(bloque);
    g_intentos = 0;
  } else {
    // Mapped but without a block. This is not taken as success: the counter keeps going up so the next
    // tick can ask for room and a session again.
    ++g_intentos;
  }

  /*
   * Reverse-NX. Its block is not created by the overlay either: it is created by the plugin SaltyNX
   * injects into the game, and it is not injected into us, so its overlay said "ReverseNX-RT is not
   * running!". We create it here like the FPS one and its overlay starts working: the player picks
   * handheld or docked and the game obeys. The overlay requires no handshake for this (it only looks
   * for the "NXRT" magic), but it does require `pluginActive` to show the controls, and that means
   * "the game has asked for the mode": it is set in ModoBase.
   */
  auto* reverse = static_cast<BloqueReverseNx*>(BuscarMarca(kMagicReverseNx));
  if (!reverse && reservada_nx) {
    reverse = reinterpret_cast<BloqueReverseNx*>(g_base + desplazamiento_nx);
    std::memset(reverse, 0, sizeof(*reverse));
    reverse->magic = kMagicReverseNx;
    reverse->por_defecto = true;  // the system decides until the player says otherwise
    AvisoNum("[saltynx] bloque de Reverse-NX creado en el desplazamiento", (long long)desplazamiento_nx);
  } else if (!g_avisado_reverse) {
    g_avisado_reverse = true;  // once, since this is retried every second
    Aviso(reverse ? "[saltynx] bloque de Reverse-NX ya presente" : "[saltynx] sin sitio para el bloque de Reverse-NX");
  }
  // Only stored if there is one. Otherwise a retry would write nullptr over a good pointer.
  if (reverse) {
    g_reverse.store(reverse, std::memory_order_release);
  }
}

void Actualizar(double fps_segundo, double fps_media, uint32_t ancho, uint32_t alto, uint64_t fotogramas) {
  // Remembered before publishing. If the block appears later (overlay opened afterwards, or SaltyNX
  // slow to hand out the memory), it is seeded with these values the moment it exists.
  const double tope = fps_segundo < 0.0 ? 0.0 : (fps_segundo > 255.0 ? 255.0 : fps_segundo);
  g_ultimo_fps = uint8_t(tope + 0.5);
  g_ultima_media = float(fps_media);
  if (ancho && alto) {
    g_ultimo_ancho = uint16_t(ancho);
    g_ultimo_alto = uint16_t(alto);
  }
  g_ultimos_fotogramas = fotogramas;

  /*
   * The block may be missing (SaltyNX was not handing out memory yet, or the allocation did not fit)
   * or may no longer be ours (the page is shared by several clients). The magic is checked on every
   * tick and, if it is missing, the block is attached again. This is what allows the overlay to be
   * opened at any time without restarting the game: retrying only while the page was unmapped meant
   * that once mapped it was never looked at again.
   */
  BloqueFps* bloque = g_fps.load(std::memory_order_acquire);
  // This starts at second zero, before the game applies its cvars, so the switch has to be checked here
  // too: if it is off, the block is released and Latir stops writing as well. Turning it back on
  // recovers it on the next tick.
  if (!g_habilitado.load(std::memory_order_relaxed)) {
    if (bloque) {
      bloque->plugin_activo = false;
      g_fps.store(nullptr, std::memory_order_release);
    }
    return;
  }
  if (!BloqueValido(bloque)) {
    Iniciar();
    bloque = g_fps.load(std::memory_order_acquire);
  }
  if (bloque) {
    // Both values, always: FPS and resolution. Leaving the resolution to Latir is not enough, because
    // Latir only runs while the game is presenting: during loading the overlay was left without the RES row.
    SembrarBloque(bloque);
  }
  if (BloqueReverseNx* reverse = g_reverse.load(std::memory_order_acquire)) {
    g_reverse_activo.store(!reverse->por_defecto, std::memory_order_relaxed);
    g_reverse_en_base.store(reverse->en_base, std::memory_order_relaxed);
  }
}

/*
 * The heartbeat, once per frame from the ring thread.
 *
 * The overlay does not believe the game is alive until it answers two handshakes, and both are very
 * short (Status-Monitor-Overlay, source/Utils.hpp and source/modes/Resolutions.hpp):
 *
 *   NxFps->pluginActive = false;  svcSleepThread(100'000'000);  if (NxFps->pluginActive) GameRunning = true;
 *   NxFps->renderCalls[0].calls = 0xFFFF;  ... if (renderCalls[0].calls != 0xFFFF) resolutionLookup = 2;
 *
 * With one update per second it was not enough: the overlay said "Game is not running or it's
 * incompatible". Also, it computes the FPS average from FPSticks[10] (frequency / average of the
 * ticks), so with the array at zero it showed "inf". Here it is filled with the real time between
 * presents.
 */
void Latir(uint32_t ancho, uint32_t alto) {
  BloqueFps* bloque = g_fps.load(std::memory_order_acquire);
  // If the magic is gone, the pointer is not valid. It is dropped and the profiler thread's one-second
  // tick recovers it: this runs on the ring thread on every frame, so no IPC and no searching here.
  if (!BloqueValido(bloque)) {
    if (bloque) {
      g_fps.store(nullptr, std::memory_order_release);
    }
    return;
  }
  bloque->plugin_activo = true;
  bloque->api = kApiVulkan;

  // If the block is a different one (just attached because the overlay was opened now), the previous
  // time is minutes old: that first measurement is discarded, or the overlay would show a very long
  // frame in the average.
  static const BloqueFps* ultimo_visto = nullptr;  // only the ring thread touches it
  if (ultimo_visto != bloque) {
    ultimo_visto = bloque;
    g_tick_previo = 0;
  }

  const uint64_t ahora = armGetSystemTick();
  if (g_tick_previo != 0) {
    const uint64_t salto = ahora - g_tick_previo;
    bloque->ticks[g_tick_pos] = uint32_t(salto > 0xFFFFFFFFull ? 0xFFFFFFFFull : salto);
    g_tick_pos = (g_tick_pos + 1) % 10;
    ++g_fotogramas;
    bloque->numero_fotograma = g_fotogramas;
  }
  g_tick_previo = ahora;

  if (ancho && alto) {
    // `calls` cannot be 0xFFFF: that is the mark the overlay asks with. It is set to the frames of the
    // last second, which is what NX-FPS counts.
    const uint16_t cuantas = bloque->fps ? bloque->fps : uint16_t(1);
    const ResolucionLlamadas r = {uint16_t(ancho), uint16_t(alto), cuantas};
    bloque->render[0] = r;
    bloque->viewport[0] = r;
  }
}

void Habilitar(bool habilitado) { g_habilitado.store(habilitado ? 1 : 0, std::memory_order_relaxed); }

bool ModoBase(bool real) {
  BloqueReverseNx* reverse = g_reverse.load(std::memory_order_acquire);
  if (!reverse) {
    return real;
  }
  // In Reverse-NX, `pluginActive` means "the game has asked for the mode"; without it its overlay says
  // "Game didn't check any mode!" and does not show the controls.
  reverse->plugin_activo = true;
  if (reverse->por_defecto) {
    reverse->en_base = real;  // the system decides: mirror it so the overlay shows the real mode
    return real;
  }
  return reverse->en_base;
}

EstadoReverseNx EstadoReverse() {
  const BloqueReverseNx* reverse = g_reverse.load(std::memory_order_acquire);
  if (!reverse) {
    return {false, false, false, false};
  }
  return {true, reverse->en_base, reverse->por_defecto, reverse->plugin_activo};
}

}  // namespace rex::ui::switch_saltynx

/* Switch from the game, before the profiler thread starts. */
extern "C" void RexSwitchSaltyNxHabilitar(int habilitado) {
  rex::ui::switch_saltynx::Habilitar(habilitado != 0);
}

#endif  // REX_PLATFORM_SWITCH
