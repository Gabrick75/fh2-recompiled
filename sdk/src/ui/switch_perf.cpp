/**
 * @file        switch_perf.cpp
 * @brief       Performance measurement on the console, without a debugger
 *
 * The first boot that reached the title screen ran at 5-10 fps, with two cores at 97% and the
 * GPU at 99.7% according to the Horizon OC monitor. That does not tell what to optimize: it takes
 * knowing which thread spends the CPU and in which function. Horizon has no perf and no debugger
 * on a retail console, so it is measured from inside. Every 10 s a report is appended to
 * <NRO folder>/logs/rex/rex_perfil.log:
 *
 *  - Game FPS: guest presents (IssueSwap).
 *  - Faults per second by type: each one costs an exception, or two with the resume through the
 *    kernel.
 *  - Exact CPU of each thread: the kernel keeps each thread's CPU time (svcGetInfo with
 *    InfoType_ThreadTickCount, 13.0.0+), with its priority, its preferred core and its core mask.
 *  - Where it is spent: sampling at 1 kHz. A high-priority thread pauses a busy thread
 *    (svcSetThreadActivity), reads its pc, its lr and its stack through the x29 chain
 *    (svcGetThreadContext3) and resumes it. The stacks also tell what a thread stopped in the
 *    kernel is waiting for. Addresses come out as image+0x...; they are translated with addr2line
 *    on the ELF.
 *
 * Threads: by wrapping libnx's threadCreate and threadClose (--wrap in rexglue_switch.cmake). Every
 * thread goes through there, pthread ones included, so Mesa's and the SDK's show up too. The name
 * comes from pthread_setname_np (switch_libc_supplement.c).
 *
 * libnx calls: also with --wrap, the calls that cost IPC or walk memory are counted, along with the
 * time the calling thread spends inside: fences (zero-timeout queries and waits), submission
 * kickoffs, new NvMaps (with and without CPU cache), GPU addresses and mappings, armDCacheClean, the
 * window queue and svcSleepThread. Every fence query is at least one ioctl even when the GPU has
 * already finished (see docs/platform-notes.md), so the count per second is needed before changing
 * anything.
 *
 * Careful with the pause: while a thread is paused nothing is done that could take a lock (no
 * malloc, no stdio): if the paused thread held it, this one would wait forever. Between pause and
 * resume there are only system calls. If pausing fails (for example because the SDK had already
 * suspended the thread) it is left alone: resuming it would break that suspension.
 *
 * It goes in the executable (rexglue_switch_startup): the SDK calls RexSwitchPerfCount from
 * libraries, and the wrappers have to be defined before libnx.a enters the link.
 */

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <switch.h>

#include "rex/ui/switch_apm.h"
#include "rex/ui/switch_saltynx.h"
#include "rex/ui/switch_sysclk.h"

extern "C" {
void _start(void);
/* rexglue log folder, ending in '/' (switch_crash_hooks.c). */
const char* RexSwitchLogDir(void);
/*
 * From the guest memory core (guest_memory_switch.cpp): how much backing is
 * committed and how much is mapped in total, counting the 360 mirrors.
 */
size_t RexGmCommittedBytes(void);
size_t RexGmMappedBytes(void);
/* 0 = sin probar, 1 = permisos (paginas vigiladas legibles), 2 = desmapeo. */
int RexGmModoProteccion(void);
Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                           size_t stack_sz, int prio, int cpuid);
Result __real_threadClose(Thread* t);
Result __real_nvFenceWait(NvFence* f, s32 timeout_us);
Result __real_nvGpuChannelKickoff(NvGpuChannel* c);
Result __real_nvMapCreate(NvMap* m, void* cpu_addr, u32 size, u32 align, NvKind kind, bool is_cpu_cacheable);
Result __real_nvAddressSpaceAllocFixed(NvAddressSpace* a, bool sparse, u64 size, iova_t iova);
Result __real_nvioctlNvhostAsGpu_MapBufferEx(u32 fd, u32 flags, u32 kind, u32 nvmap_handle, u32 page_size,
                                             u64 buffer_offset, u64 mapping_size, u64 input_offset, u64* offset);
void __real_armDCacheClean(void* addr, size_t size);
Result __real_nwindowQueueBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence);
Result __real_bqDequeueBuffer(Binder* b, bool async, u32 width, u32 height, s32 format, u32 usage, s32* buf,
                              NvMultiFence* fence);
void __real_svcSleepThread(s64 nano);
}

namespace {

constexpr size_t kMaxThreads = 160;
constexpr u64 kSampleIntervalNs = 1000000ULL;  // 1 ms
constexpr u64 kSegundoNs = 1000000000ULL;      // tick for the console overlays

/* internal resolution published to the overlays. The game sets it when choosing the video mode. */
std::atomic<uint32_t> g_resolucion{(1280u << 16) | 720u};
constexpr u64 kPassiveIntervalNs = 100000000ULL;  // 100 ms, without pausing threads
// Next to the rexglue logs, in <NRO folder>/logs/rex/.
std::string StackFlagPath() {
  return std::string(RexSwitchLogDir()) + "perfil_pilas.flag";
}
constexpr u64 kReportSeconds = 10;
/*
 * Let the game start first. This only delays stack sampling and the first report, which read the
 * SD and pause threads. The console overlays (FPS and resolution) are published from second zero:
 * see the warm-up loop in ProfilerMain.
 */
constexpr u64 kStartDelayNs = 8000000000ULL;
constexpr size_t kMaxSamples = 1 << 15;
constexpr size_t kFrames = 10;
constexpr unsigned kCounterCount = 33;
// In <NRO folder>/logs/rex/ (switch_crash_hooks.c computes it at startup).
std::string ReportPath() {
  return std::string(RexSwitchLogDir()) + "rex_perfil.log";
}

struct Slot {
  std::atomic<u32> handle{0};
  char name[32]{};
  // Only the profiler thread touches these:
  u32 seen = 0;
  u64 last_ticks = 0;
  bool busy = true;
  double cpu = 0.0;
};
Slot g_slots[kMaxThreads];

struct Sample {
  u64 pc;
  u64 lr;
  u64 frames[kFrames];  // return addresses along the x29 chain
  u16 slot;
  u64 tick;  // when it was taken (armGetSystemTick), to separate those from long frames
};

/*
 * Long frame windows. The renderer calls RexSwitchPerfTiron with the interval of every frame over
 * 45 ms; the report separates the samples taken inside those intervals and says what each thread was
 * doing right then (section "durante los tirones"). Only works with perfil_pilas.flag.
 */
constexpr size_t kMaxTirones = 256;
struct VentanaTiron {
  u64 inicio;
  u64 fin;
};
VentanaTiron g_tirones[kMaxTirones];
std::atomic<u32> g_num_tirones{0};
Sample g_samples[kMaxSamples];
size_t g_sample_count = 0;

std::atomic<u64> g_counters[kCounterCount];

/*
 * Wrapped libnx calls (see the file header): how many, how much time inside and, where it makes
 * sense, how many bytes. They are constant-initialized, so they are valid even if someone calls
 * before the constructors.
 */
enum : unsigned {
  kFenceConsulta,     // nvFenceWait con espera 0
  kFenceEspera,       // nvFenceWait con espera
  kKickoff,           // nvGpuChannelKickoff
  kNvMapCacheada,     // nvMapCreate with CPU cache
  kNvMapSinCache,     // nvMapCreate without CPU cache
  kReservaDireccion,  // nvAddressSpaceAllocFixed
  kMapeoGpu,          // nvioctlNvhostAsGpu_MapBufferEx
  kCacheClean,        // armDCacheClean
  kQueueBuffer,       // nwindowQueueBuffer
  kDequeueBuffer,     // bqDequeueBuffer
  kSleep0,            // svcSleepThread(0)
  kSleepCeder,        // svcSleepThread(-1 o -2)
  kSleepCorto,        // up to 1 ms
  kSleepLargo,        // over 1 ms
  kLlamadasCount,
};
struct Llamadas {
  std::atomic<u64> n{0};
  std::atomic<u64> ticks{0};
  std::atomic<u64> bytes{0};
};
Llamadas g_llamadas[kLlamadasCount];

inline void Anotar(unsigned id, u64 desde, u64 bytes = 0) {
  const u64 ticks = armGetSystemTick() - desde;
  Llamadas& l = g_llamadas[id];
  l.n.fetch_add(1, std::memory_order_relaxed);
  l.ticks.fetch_add(ticks, std::memory_order_relaxed);
  if (bytes) {
    l.bytes.fetch_add(bytes, std::memory_order_relaxed);
  }
}

/*
 * Automatic A/B tests. Each report (10 s) removes one part of the GPU work: those commands (draw or
 * dispatch) stop being recorded, but everything else stays the same, so the state does not break.
 * The image looks wrong while the mode lasts; that is expected. If with "sin nada" it is just as
 * slow, the cost is not in the work but in something fixed (submissions, driver).
 */
struct Mode {
  u32 mask;
  const char* name;
};
constexpr Mode kModes[] = {
    {0, "normal"},
    {1, "sin dibujados"},
    {2, "sin transferencias"},
    {4, "sin resolves"},
    {8, "sin cargas de texturas"},
    {16, "sin efecto de presentacion"},
    {32, "sin despacho por baldosas"},
    {63, "sin nada"},
};
constexpr size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

/*
 * A/B tests, started by hand
 *
 * They used to be enabled with this constant and rotate on their own from startup. That is no good:
 * the game takes a while to reach a race, and on the way it spent its time in modes with drawing
 * turned off. Once they were left on by mistake and the game did not even reach the title screen.
 *
 * Now the player starts the rotation with L+R+ZL from inside the race, which is the only place where
 * the measurement is useful. From then on it removes one part of the GPU work per report, goes once
 * through the eight modes and returns to "normal".
 *
 * How to read it. If with "sin nada" the game is just as slow, the cost is not in the GPU work but
 * in something fixed: submissions, the driver, or the guest itself. If FPS shoots up, the mode where
 * it rises most points to the culprit.
 */
std::atomic<bool> g_ab_activas{false};
std::atomic<u32> g_skip_mask{0};

extern "C" void RexSwitchPerfToggleAb(void) {
  const bool nuevo = !g_ab_activas.load(std::memory_order_relaxed);
  g_ab_activas.store(nuevo, std::memory_order_relaxed);
  if (!nuevo) {
    g_skip_mask.store(0, std::memory_order_relaxed);
  }
}

/*
 * Horizon limits how much memory a process may map (LimitableResource_Memory). Our
 * guest memory model maps each chunk several times (the shadow and once per 360
 * view), so that limit is the real ceiling, not free memory. When it runs out,
 * even committing 4 KB fails with 2001-0103 (resource exhausted).
 */
void LimiteDeMapeo(u64* usado_mb, u64* tope_mb, u64* proceso_mb, u64* total_mb) {
  *usado_mb = *tope_mb = *proceso_mb = *total_mb = 0;
  u64 v = 0;
  if (R_SUCCEEDED(svcGetInfo(&v, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0))) {
    *proceso_mb = v >> 20;
  }
  if (R_SUCCEEDED(svcGetInfo(&v, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) {
    *total_mb = v >> 20;
  }
  Handle reslimit = INVALID_HANDLE;
  if (R_FAILED(svcGetInfo(&v, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0))) {
    return;
  }
  reslimit = static_cast<Handle>(v);
  s64 actual = 0, tope = 0;
  if (R_SUCCEEDED(svcGetResourceLimitCurrentValue(&actual, reslimit, LimitableResource_Memory))) {
    *usado_mb = static_cast<u64>(actual) >> 20;
  }
  if (R_SUCCEEDED(svcGetResourceLimitLimitValue(&tope, reslimit, LimitableResource_Memory))) {
    *tope_mb = static_cast<u64>(tope) >> 20;
  }
  svcCloseHandle(reslimit);
}

u64 g_base = 0, g_text_lo = 0, g_text_hi = 0;
Thread g_thread;

void Register(Handle h) {
  if (!h) {
    return;
  }
  for (auto& s : g_slots) {
    u32 expected = 0;
    if (s.handle.compare_exchange_strong(expected, h)) {
      s.name[0] = 0;
      return;
    }
  }
}

void Unregister(Handle h) {
  if (!h) {
    return;
  }
  for (auto& s : g_slots) {
    u32 expected = h;
    if (s.handle.compare_exchange_strong(expected, 0)) {
      return;
    }
  }
}

bool InText(u64 addr) { return addr >= g_text_lo && addr < g_text_hi; }

void PrintAddr(FILE* f, u64 addr) {
  if (InText(addr)) {
    std::fprintf(f, "imagen+0x%" PRIx64, addr - g_base);
  } else {
    std::fprintf(f, "0x%016" PRIx64, addr);
  }
}

/*
 * Is the pc right after a system call? Then the thread is waiting in the kernel,
 * not using CPU.
 */
bool AfterSvc(u64 pc) {
  if (!InText(pc) || pc < g_text_lo + 4) {
    return false;
  }
  const u32 insn = *reinterpret_cast<const u32*>(pc - 4);
  return (insn & 0xFFE0001Fu) == 0xD4000001u;
}

struct Count {
  u64 addr;
  u32 n;
};

std::vector<Count> Histogram(std::vector<u64>& addrs) {
  std::sort(addrs.begin(), addrs.end());
  std::vector<Count> out;
  for (u64 a : addrs) {
    if (out.empty() || out.back().addr != a) {
      out.push_back({a, 1});
    } else {
      ++out.back().n;
    }
  }
  std::sort(out.begin(), out.end(), [](const Count& x, const Count& y) { return x.n > y.n; });
  return out;
}

void Report(u64 elapsed_ticks, u64 tick_freq, u64 counters_last[kCounterCount],
            const char* mode) {
  const double seconds = double(elapsed_ticks) / double(tick_freq);

  u64 counters_now[kCounterCount];
  for (unsigned i = 0; i < kCounterCount; ++i) {
    counters_now[i] = g_counters[i].load(std::memory_order_relaxed);
  }

  // CPU of each thread over the interval, as % of one core.
  std::vector<size_t> order;
  for (size_t i = 0; i < kMaxThreads; ++i) {
    Slot& s = g_slots[i];
    const u32 h = s.handle.load();
    if (!h) {
      s.seen = 0;
      continue;
    }
    u64 ticks = 0;
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, h, UINT64_MAX))) {
      continue;
    }
    if (s.seen != h) {  // new thread: no interval yet
      s.seen = h;
      s.last_ticks = ticks;
      s.busy = true;
      s.cpu = 0.0;
      continue;
    }
    s.cpu = double(ticks - s.last_ticks) * 100.0 / double(elapsed_ticks);
    s.last_ticks = ticks;
    /*
     * All threads are sampled, including those that use no CPU. In a hang caused by
     * a wait, the thread that matters is exactly that one: the stopped one.
     */
    s.busy = true;
    order.push_back(i);
  }
  std::sort(order.begin(), order.end(),
            [](size_t a, size_t b) { return g_slots[a].cpu > g_slots[b].cpu; });

  FILE* f = std::fopen(ReportPath().c_str(), "a");
  if (!f) {
    g_sample_count = 0;
    return;
  }

  u64 lim_usado = 0, lim_tope = 0, proc_usado = 0, proc_total = 0;
  LimiteDeMapeo(&lim_usado, &lim_tope, &proc_usado, &proc_total);

  double total_cpu = 0.0;
  for (size_t i : order) {
    total_cpu += g_slots[i].cpu;
  }
  std::fprintf(f,
               "==== %.1f s | modo: %s | juego %.1f fps | CPU total %.0f%% (400%% = 4 nucleos) | fallos/s: "
               "lectura emulada %.0f, manejador SDK %.0f, reintento emulado %.0f, SEH %.0f, "
               "fisica confirmada %.0f, vistas %.0f | invitado %zu/%zu MB (respaldo/mapeado) | "
               "limite de mapeo %llu/%llu MB, proceso %llu/%llu MB | "
               "muestras %zu | vigilancia: %s | samplers: %.0f nuevos/s, %.0f parones/s\n",
               seconds, mode, double(counters_now[0] - counters_last[0]) / seconds, total_cpu,
               double(counters_now[1] - counters_last[1]) / seconds,
               double(counters_now[2] - counters_last[2]) / seconds,
               double(counters_now[3] - counters_last[3]) / seconds,
               double(counters_now[4] - counters_last[4]) / seconds,
               double(counters_now[17] - counters_last[17]) / seconds,
               double(counters_now[18] - counters_last[18]) / seconds,
               RexGmCommittedBytes() >> 20, RexGmMappedBytes() >> 20,
               (u64)lim_usado, (u64)lim_tope, (u64)proc_usado, (u64)proc_total, g_sample_count,
                (RexGmModoProteccion() == 1   ? "permisos (paginas legibles)"
                 : RexGmModoProteccion() == 2 ? "desmapeo (cada lectura falla)"
                                              : "sin probar"),
                double(counters_now[20] - counters_last[20]) / seconds,
                double(counters_now[19] - counters_last[19]) / seconds);
  // Work the game sends to the GPU, per presented frame. One screen is
  // 1280x720 = 921,600 pixels.
  const auto delta = [&](unsigned id) { return double(counters_now[id] - counters_last[id]); };
  const double frames = delta(0);
  // Mix clipping before conversion to 16 bits (21 samples above 1.0, 22 buffers with a peak of 0.98 or
  // more, 23 maximum peak of the interval in ten-thousandths) and the console mode, for the crackling
  // in handheld mode.
  const u64 pico_audio = g_counters[23].exchange(0, std::memory_order_relaxed);
  std::fprintf(f,
               "     audio: %.0f bloques de cliente mezclados, %.0f solicitudes sin datos, "
               "%.0f/%.0f buffers audout con PCM no nulo, %.0f muestras saturadas (mas de 1,0 antes de recortar), "
               "%.0f buffers con pico de 0,98 o mas, pico maximo %.3f | consola en modo %s\n",
               delta(24), delta(25), delta(26), delta(27), delta(21), delta(22), double(pico_audio) / 10000.0,
               rex::ui::switch_saltynx::ModoBase(appletGetOperationMode() == AppletOperationMode_Console)
                   ? "sobremesa"
                   : "portatil");
  // Reverse-NX state as is, without interpretation. A whole test session was wasted because its
  // overlay said "Docked" while "Controlled by system" was Yes, and in that case its mode does not
  // rule: it only mirrors the console's. This shows at a glance which of the two is happening.
  {
    const auto reverse = rex::ui::switch_saltynx::EstadoReverse();
    const bool real = appletGetOperationMode() == AppletOperationMode_Console;
    if (!reverse.hay) {
      std::fprintf(f, "     Reverse-NX: sin bloque (consola %s, manda ella)\n",
                   real ? "en la base" : "en las manos");
    } else {
      std::fprintf(f,
                   "     Reverse-NX: dice %s, manda %s (Controlled by system %s), el juego ha preguntado: %s; "
                   "consola de verdad %s -> se obedece %s\n",
                   reverse.en_base ? "sobremesa" : "portatil", reverse.por_defecto ? "la consola" : "Reverse-NX",
                   reverse.por_defecto ? "Yes" : "No", reverse.plugin_activo ? "si" : "no",
                   real ? "en la base" : "en las manos",
                   rex::ui::switch_saltynx::ModoBase(real) ? "sobremesa" : "portatil");
    }
  }
  // Real console clocks (clkrst, 8.0.0+) and the cores the process may use. It tells whether the
  // session ran overclocked and with which CPU limit it was measured: performance tests run without
  // overclock.
  {
    static bool iniciado = false;
    static bool hay_clkrst = false;
    static ClkrstSession sesiones[3]{};
    static u64 mascara_nucleos = 0;
    if (!iniciado) {
      iniciado = true;
      if (R_FAILED(svcGetInfo(&mascara_nucleos, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        mascara_nucleos = 0;
      }
      if (R_SUCCEEDED(clkrstInitialize())) {
        static const PcvModuleId modulos[3] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
        hay_clkrst = true;
        for (unsigned i = 0; i < 3; ++i) {
          if (R_FAILED(clkrstOpenSession(&sesiones[i], modulos[i], 3))) {
            hay_clkrst = false;
          }
        }
      }
    }
    u32 hz[3] = {0, 0, 0};
    if (hay_clkrst) {
      for (unsigned i = 0; i < 3; ++i) {
        clkrstGetClockRate(&sesiones[i], &hz[i]);
      }
    }
    unsigned nucleos = 0;
    for (unsigned i = 0; i < 64; ++i) {
      nucleos += unsigned((mascara_nucleos >> i) & 1);
    }
    // SoC and board temperature (ts service, sessions on [10.0.0+]). The system lowers clocks around
    // 70 degrees and Erista gets there before Mariko: without this, "it is slow" cannot be told apart
    // from "it is hot". On the OLED model the board sensor reads wrong, because it sits next to the
    // charging IC.
    static bool ts_iniciado = false;
    static bool hay_ts = false;
    static TsSession ts_soc{}, ts_placa{};
    if (!ts_iniciado) {
      ts_iniciado = true;
      if (R_SUCCEEDED(tsInitialize())) {
        hay_ts = R_SUCCEEDED(tsOpenSession(&ts_soc, TsDeviceCode_LocationExternal)) &&
                 R_SUCCEEDED(tsOpenSession(&ts_placa, TsDeviceCode_LocationInternal));
      }
    }
    float grados_soc = 0.0f, grados_placa = 0.0f;
    if (hay_ts) {
      tsSessionGetTemperature(&ts_soc, &grados_soc);
      tsSessionGetTemperature(&ts_placa, &grados_placa);
    }
    std::fprintf(f,
                 "     relojes: CPU %.1f MHz, GPU %.1f MHz, memoria %.1f MHz%s | nucleos del proceso %u "
                 "(mascara 0x%llX) | SoC %.1f C, placa %.1f C%s\n",
                 double(hz[0]) / 1.0e6, double(hz[1]) / 1.0e6, double(hz[2]) / 1.0e6,
                 hay_clkrst ? "" : " (clkrst no disponible)", nucleos,
                 (unsigned long long)mascara_nucleos, double(grados_soc), double(grados_placa),
                 hay_ts ? "" : " (ts no disponible)");
  }
  // libnx calls: per second, with the ms per second the calling threads spend inside.
  {
    static u64 ultimo[kLlamadasCount][3];
    double n[kLlamadasCount], ms[kLlamadasCount], mb[kLlamadasCount];
    for (unsigned i = 0; i < kLlamadasCount; ++i) {
      const u64 vn = g_llamadas[i].n.load(std::memory_order_relaxed);
      const u64 vt = g_llamadas[i].ticks.load(std::memory_order_relaxed);
      const u64 vb = g_llamadas[i].bytes.load(std::memory_order_relaxed);
      n[i] = double(vn - ultimo[i][0]) / seconds;
      ms[i] = double(vt - ultimo[i][1]) * 1000.0 / double(tick_freq) / seconds;
      mb[i] = double(vb - ultimo[i][2]) / 1048576.0 / seconds;
      ultimo[i][0] = vn;
      ultimo[i][1] = vt;
      ultimo[i][2] = vb;
    }
    std::fprintf(f,
                 "     libnx por segundo (llamadas y ms dentro): fences consultadas %.0f (%.1f ms), esperadas %.0f "
                 "(%.1f ms) | kickoff %.0f (%.1f ms) | NvMap nuevos %.1f con cache (%.1f MB) y %.1f sin cache "
                 "(%.1f MB), %.1f ms | direcciones de GPU %.1f (%.1f ms), mapeos %.1f (%.1f MB, %.1f ms) | "
                 "armDCacheClean %.0f (%.1f MB, %.1f ms) | cola de la ventana %.1f (%.1f ms), sacar buffer %.1f "
                 "(%.1f ms) | svcSleepThread: 0 %.0f, ceder %.0f, hasta 1 ms %.0f (%.1f ms), mas %.0f\n",
                 n[kFenceConsulta], ms[kFenceConsulta], n[kFenceEspera], ms[kFenceEspera], n[kKickoff],
                 ms[kKickoff], n[kNvMapCacheada], mb[kNvMapCacheada], n[kNvMapSinCache], mb[kNvMapSinCache],
                 ms[kNvMapCacheada] + ms[kNvMapSinCache], n[kReservaDireccion], ms[kReservaDireccion],
                 n[kMapeoGpu], mb[kMapeoGpu], ms[kMapeoGpu], n[kCacheClean], mb[kCacheClean], ms[kCacheClean],
                 n[kQueueBuffer], ms[kQueueBuffer], n[kDequeueBuffer], ms[kDequeueBuffer], n[kSleep0],
                 n[kSleepCeder], n[kSleepCorto], ms[kSleepCorto], n[kSleepLargo]);
  }
  if (frames > 0) {
    std::fprintf(f,
                 "     por fotograma: %.0f dibujados | tijera %.1f pantallas | %.1f envios | "
                 "%.1f resolves (%.2f pantallas) | transferencias: %.1f llamadas, %.1f render "
                 "targets, %.1f dibujados | %.1f texturas cargadas | %.2f MB de memoria "
                 "compartida subida | pipelines creados en el intervalo: %.0f\n",
                 delta(5) / frames, delta(6) / frames / 921600.0, delta(7) / frames,
                 delta(9) / frames, delta(10) / frames / 921600.0, delta(11) / frames,
                 delta(12) / frames, delta(13) / frames, delta(14) / frames,
                 delta(15) / frames / 1048576.0, delta(16));
    std::fprintf(f,
                 "     dibujados por ancho de superficie: 1600+ %.0f | 1280-1599 %.0f | "
                 "640-1279 %.0f | 256-639 %.0f | menos de 256 %.0f\n",
                 delta(28) / frames, delta(29) / frames, delta(30) / frames, delta(31) / frames,
                 delta(32) / frames);
  }

  for (unsigned i = 0; i < kCounterCount; ++i) {
    counters_last[i] = counters_now[i];
  }

  std::vector<Sample> samples(g_samples, g_samples + g_sample_count);
  g_sample_count = 0;
  std::sort(samples.begin(), samples.end(),
            [](const Sample& a, const Sample& b) { return a.slot < b.slot; });

  // What each thread was doing during the long frames of this interval.
  {
    const u32 nt = std::min<u32>(g_num_tirones.exchange(0), kMaxTirones);
    if (nt && !samples.empty()) {
      std::vector<VentanaTiron> v(g_tirones, g_tirones + nt);
      std::sort(v.begin(), v.end(), [](const VentanaTiron& a, const VentanaTiron& b) { return a.inicio < b.inicio; });
      double ms_total = 0.0;
      for (const VentanaTiron& w : v) {
        ms_total += double(w.fin - w.inicio) * 1000.0 / double(tick_freq);
      }
      const auto dentro = [&v](u64 t) {
        auto it = std::upper_bound(v.begin(), v.end(), t, [](u64 x, const VentanaTiron& w) { return x < w.inicio; });
        return it != v.begin() && t <= (it - 1)->fin;
      };
      std::fprintf(f, "\n== durante los tirones: %u fotogramas de mas de 45 ms (%.0f ms en total) ==\n", nt, ms_total);
      for (size_t i : order) {
        std::vector<u64> pcs;
        std::vector<const Sample*> todas;
        size_t en_svc = 0;
        for (const Sample& m : samples) {
          if (m.slot != i || !dentro(m.tick)) {
            continue;
          }
          todas.push_back(&m);
          if (AfterSvc(m.pc)) {
            ++en_svc;
          } else {
            pcs.push_back(m.pc);
          }
        }
        if (todas.size() < 5) {
          continue;
        }
        const Slot& s = g_slots[i];
        std::fprintf(f, "-- hilo \"%s\": %zu muestras en los tirones, %.0f%% esperando en el kernel\n",
                     s.name[0] ? s.name : "?", todas.size(), double(en_svc) * 100.0 / double(todas.size()));
        const auto h = Histogram(pcs);
        for (size_t k = 0; k < h.size() && k < 25; ++k) {
          std::fprintf(f, "   %5.1f%%  pc ", double(h[k].n) * 100.0 / double(todas.size()));
          PrintAddr(f, h[k].addr);
          std::fputc('\n', f);
        }
        std::sort(todas.begin(), todas.end(), [](const Sample* a, const Sample* b) {
          return std::memcmp(a->frames, b->frames, sizeof(a->frames)) < 0;
        });
        std::vector<std::pair<const Sample*, u32>> pilas;
        for (const Sample* m : todas) {
          if (pilas.empty() || std::memcmp(pilas.back().first->frames, m->frames, sizeof(m->frames)) != 0) {
            pilas.push_back({m, 1});
          } else {
            ++pilas.back().second;
          }
        }
        std::sort(pilas.begin(), pilas.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t k = 0; k < pilas.size() && k < 10; ++k) {
          std::fprintf(f, "   %5.1f%%  pila", double(pilas[k].second) * 100.0 / double(todas.size()));
          for (size_t j = 0; j < kFrames && pilas[k].first->frames[j]; ++j) {
            std::fputs(j == 0 ? " " : " <- ", f);
            PrintAddr(f, pilas[k].first->frames[j]);
          }
          std::fputc('\n', f);
        }
      }
    }
  }

  for (size_t i : order) {
    const Slot& s = g_slots[i];
    /*
     * Threads that use no CPU are listed too, with less detail: in a hang their
     * stacks are needed to know what they are waiting for.
     */
    const bool activo = s.cpu >= 1.0;
    u64 tid = 0;
    svcGetThreadId(&tid, s.handle.load());
    // Horizon priority, preferred core and allowed cores: who can take the CPU from whom.
    s32 prioridad = -1;
    s32 preferido = -1;
    u64 mascara = 0;
    svcGetThreadPriority(&prioridad, s.handle.load());
    svcGetThreadCoreMask(&preferido, &mascara, s.handle.load());
    std::fprintf(f,
                 "\n-- hilo %" PRIu64 " \"%s\": CPU %.1f%% | prioridad 0x%X, nucleo preferido %d, "
                 "mascara 0x%llX\n",
                 tid, s.name[0] ? s.name : "?", s.cpu, static_cast<unsigned>(prioridad),
                 static_cast<int>(preferido), static_cast<unsigned long long>(mascara));

    auto lo = std::lower_bound(samples.begin(), samples.end(), i,
                               [](const Sample& a, size_t v) { return a.slot < v; });
    auto hi = std::upper_bound(samples.begin(), samples.end(), i,
                               [](size_t v, const Sample& a) { return v < a.slot; });
    const size_t n = size_t(hi - lo);
    if (n == 0) {
      std::fprintf(f, "   sin muestras\n");
      continue;
    }
    std::vector<u64> pcs, lrs;
    size_t in_svc = 0;
    for (auto it = lo; it != hi; ++it) {
      if (AfterSvc(it->pc)) {
        ++in_svc;
        continue;
      }
      pcs.push_back(it->pc);
      lrs.push_back(it->lr);
    }
    std::fprintf(f, "   %zu muestras, %.0f%% esperando en el kernel\n", n,
                 double(in_svc) * 100.0 / double(n));
    const auto pc_hist = Histogram(pcs);
    const size_t busy = pcs.size();
    for (size_t k = 0; k < pc_hist.size() && k < (activo ? 80u : 3u); ++k) {
      std::fprintf(f, "   %5.1f%%  pc ", double(pc_hist[k].n) * 100.0 / double(busy));
      PrintAddr(f, pc_hist[k].addr);
      std::fputc('\n', f);
    }
    const auto lr_hist = Histogram(lrs);
    for (size_t k = 0; k < lr_hist.size() && k < (activo ? 40u : 3u); ++k) {
      std::fprintf(f, "   %5.1f%%  lr ", double(lr_hist[k].n) * 100.0 / double(busy));
      PrintAddr(f, lr_hist[k].addr);
      std::fputc('\n', f);
    }

    // Full stacks, waits included: they say what the thread is waiting for.
    std::vector<const Sample*> by_stack;
    for (auto it = lo; it != hi; ++it) {
      by_stack.push_back(&*it);
    }
    std::sort(by_stack.begin(), by_stack.end(), [](const Sample* a, const Sample* b) {
      return std::memcmp(a->frames, b->frames, sizeof(a->frames)) < 0;
    });
    struct StackCount {
      const Sample* s;
      u32 n;
    };
    std::vector<StackCount> stacks;
    for (const Sample* s : by_stack) {
      if (stacks.empty() ||
          std::memcmp(stacks.back().s->frames, s->frames, sizeof(s->frames)) != 0) {
        stacks.push_back({s, 1});
      } else {
        ++stacks.back().n;
      }
    }
    std::sort(stacks.begin(), stacks.end(),
              [](const StackCount& a, const StackCount& b) { return a.n > b.n; });
    for (size_t k = 0; k < stacks.size() && k < (activo ? 30u : 3u); ++k) {
      std::fprintf(f, "   %5.1f%%  pila", double(stacks[k].n) * 100.0 / double(n));
      for (size_t j = 0; j < kFrames && stacks[k].s->frames[j]; ++j) {
        std::fputs(j == 0 ? " " : " <- ", f);
        PrintAddr(f, stacks[k].s->frames[j]);
      }
      std::fputc('\n', f);
    }
  }
  std::fputc('\n', f);
  std::fclose(f);
}

/*
 * The one-second tick for the console overlays, separate from the profile loop.
 *
 * Two loops use it: the warm-up one (from second zero) and the normal one (from kStartDelayNs on).
 * When it lived only in the normal loop there was neither FPS nor resolution until 8 seconds had
 * passed: they appeared with the EA logo instead of with the Most Wanted logo, which comes much
 * earlier.
 *
 * It is cheap: two counter reads and a few bytes in shared memory. Actualizar also re-attaches the
 * block if it is missing, so the overlay can be opened at any time.
 */
struct TicOverlay {
  u64 tic = 0;
  u64 presentados = 0;
  double media[10] = {};
  size_t pos = 0;

  void Arrancar(u64 ahora) {
    tic = ahora;
    presentados = g_counters[0].load(std::memory_order_relaxed);
    // Publish what is known right away: the resolution is valid from the first instant (g_resolucion
    // starts at 1280x720 and the game corrects it when choosing the video mode), so the overlay's RES
    // row can show up even before the first frame.
    const uint32_t res = g_resolucion.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Actualizar(0.0, 0.0, res >> 16, res & 0xFFFF, presentados);
  }

  // Once per second. `con_relojes` turns off the Reverse-NX and clock sysmodule part, which is better
  // left alone during warm-up.
  void Paso(u64 ahora, u64 freq, bool con_relojes) {
    if (ahora - tic < freq) {
      return;
    }
    const double segundos = double(ahora - tic) / double(freq);
    const u64 ahora_presentados = g_counters[0].load(std::memory_order_relaxed);
    const double fps = segundos > 0.0 ? double(ahora_presentados - presentados) / segundos : 0.0;
    media[pos] = fps;
    pos = (pos + 1) % 10;
    double suma = 0.0;
    size_t cuantos = 0;
    for (double v : media) {
      if (v > 0.0) {
        suma += v;
        ++cuantos;
      }
    }
    const uint32_t res = g_resolucion.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Actualizar(fps, cuantos ? suma / double(cuantos) : fps, res >> 16, res & 0xFFFF,
                                        ahora_presentados);
    // Reverse-NX is also told that the game keeps asking for the mode (its overlay requires it to show
    // the controls) and, if the system rules, the real mode is mirrored.
    // And if requested, the clock sysmodule is asked for its docked clocks.
    if (con_relojes) {
      const bool sobremesa_real = appletGetOperationMode() == AppletOperationMode_Console;
      const bool sobremesa = rex::ui::switch_saltynx::ModoBase(sobremesa_real);
      rex::ui::switch_sysclk::SeguirModo(sobremesa, sobremesa_real);
    }
    /*
     * The memory clock is watched here too. It is outside the if above on purpose: the
     * system raises the EMC right after we request the GPU profile, that is, during
     * warm-up, which is when con_relojes is false. It is one clkrst read per second for
     * the first 20 and one every 30 after that; when we have not changed anything it
     * does not even do that.
     */
    RexSwitchApmVigilar();
    presentados = ahora_presentados;
    tic = ahora;
  }
};

void ProfilerMain(void*) {
  const u64 tick_freq = armGetSystemTickFreq();

  /*
   * The overlay first, without waiting for kStartDelayNs. Those 8 seconds are for stack sampling and
   * the first report (which read the SD and pause threads), not for this: publishing FPS and
   * resolution is a few bytes in shared memory, and it has to show already at the Most Wanted logo.
   * During warm-up the tick keeps running every second, so attaching the block is also retried if
   * SaltyNX is slow.
   */
  TicOverlay overlay;
  const u64 inicio = armGetSystemTick();
  overlay.Arrancar(inicio);
  const u64 calentamiento = (kStartDelayNs / 1000000ULL) * tick_freq / 1000ULL;
  while (armGetSystemTick() - inicio < calentamiento) {
    __real_svcSleepThread(kPassiveIntervalNs);
    overlay.Paso(armGetSystemTick(), tick_freq, false);
  }

  // Invasive sampling is only enabled explicitly, for diagnostics.
  // The normal mode keeps FPS, CPU and counters, without stopping the game.
  bool sample_stacks = false;
  if (FILE* flag = std::fopen(StackFlagPath().c_str(), "r")) {
    sample_stacks = true;
    std::fclose(flag);
  }

  u64 counters_last[kCounterCount]{};
  for (unsigned i = 0; i < kCounterCount; ++i) {
    counters_last[i] = g_counters[i].load(std::memory_order_relaxed);
  }
  if (FILE* f = std::fopen(ReportPath().c_str(), "w")) {
    std::fprintf(f, "imagen 0x%016" PRIx64 ", codigo hasta 0x%016" PRIx64 "\n\n", g_base,
                 g_text_hi);
    std::fprintf(f, "Muestreo de pilas: %s\n", sample_stacks ? "activo (1 ms)" : "desactivado");
    std::fclose(f);
  }

  // The console overlays read from SaltyNX's shared memory. That is already running from second
  // zero (see the warm-up loop above); only the normal loop continues here.

  // A short first report just to set the starting CPU time.
  u64 report_start = armGetSystemTick();
  Report(1, tick_freq, counters_last, "arranque");
  report_start = armGetSystemTick();

  size_t rr = 0;
  size_t mode_index = 0;
  while (true) {
    __real_svcSleepThread(sample_stacks ? kSampleIntervalNs : kPassiveIntervalNs);

    for (size_t tries = 0; sample_stacks && tries < kMaxThreads; ++tries) {
      const size_t idx = rr;
      rr = (rr + 1) % kMaxThreads;
      Slot& s = g_slots[idx];
      const u32 h = s.handle.load();
      if (!h || !s.busy) {
        continue;
      }
      if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) {
        break;
      }
      ThreadContext ctx;
      const Result rc = svcGetThreadContext3(&ctx, h);
      if (R_SUCCEEDED(rc) && g_sample_count < kMaxSamples) {
        Sample& out = g_samples[g_sample_count++];
        out = {};
        out.pc = ctx.pc.x;
        out.lr = ctx.lr;
        out.slot = static_cast<u16>(idx);
        out.tick = armGetSystemTick();
        // The stack, with the thread still paused: it is our own memory and
        // there are no locks. Only frames within the region of its sp.
        MemoryInfo smi;
        u32 spi = 0;
        if (R_SUCCEEDED(svcQueryMemory(&smi, &spi, ctx.sp)) && (smi.perm & Perm_R)) {
          const u64 lo = smi.addr, hi = smi.addr + smi.size;
          u64 fp = ctx.fp;
          for (size_t k = 0; k < kFrames && fp >= lo && fp + 16 <= hi && (fp & 7) == 0; ++k) {
            out.frames[k] = reinterpret_cast<const u64*>(fp)[1];
            const u64 next = reinterpret_cast<const u64*>(fp)[0];
            if (next <= fp) {
              break;
            }
            fp = next;
          }
        }
      }
      svcSetThreadActivity(h, ThreadActivity_Runnable);
      break;
    }

    const u64 now = armGetSystemTick();
    // Once per second, FPS and resolution for the overlay (the profile report runs every 10 s).
    overlay.Paso(now, tick_freq, true);
    if (now - report_start >= kReportSeconds * tick_freq) {
      Report(now - report_start, tick_freq, counters_last, kModes[mode_index].name);
      if (g_ab_activas.load(std::memory_order_relaxed)) {
        mode_index = (mode_index + 1) % kModeCount;
        g_skip_mask.store(kModes[mode_index].mask, std::memory_order_relaxed);
        if (mode_index == 0) {
          // Full cycle: it turns itself off and the game looks right again.
          g_ab_activas.store(false, std::memory_order_relaxed);
        }
      }
      report_start = armGetSystemTick();
    }
  }
}

/* Priority 102: after switch_crash_hooks.c, before the SDK. */
__attribute__((constructor(102))) void StartProfiler() {
  g_base = reinterpret_cast<u64>(&_start);
  MemoryInfo mi;
  u32 page_info = 0;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &page_info, g_base))) {
    g_text_lo = mi.addr;
    g_text_hi = mi.addr + mi.size;
  }
  Register(envGetMainThreadHandle());
  std::strncpy(g_slots[0].name, "main (interfaz)", sizeof(g_slots[0].name) - 1);
  // 0x2A: above everything in the game (audio runs at 0x2B), so the samples
  // are taken on time. It sleeps almost all the time.
  if (R_SUCCEEDED(__real_threadCreate(&g_thread, ProfilerMain, nullptr, nullptr, 0x10000, 0x2A,
                                      -2))) {
    threadStart(&g_thread);
  }
}

}  // namespace

extern "C" {

Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                           size_t stack_sz, int prio, int cpuid) {
  const Result rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
  if (R_SUCCEEDED(rc) && t) {
    Register(t->handle);
  }
  return rc;
}

Result __wrap_threadClose(Thread* t) {
  if (t) {
    Unregister(t->handle);
  }
  return __real_threadClose(t);
}

/* libnx calls that cost IPC or walk memory. See g_llamadas. */
Result __wrap_nvFenceWait(NvFence* f, s32 timeout_us) {
  const u64 desde = armGetSystemTick();
  const Result rc = __real_nvFenceWait(f, timeout_us);
  Anotar(timeout_us == 0 ? kFenceConsulta : kFenceEspera, desde);
  return rc;
}

Result __wrap_nvGpuChannelKickoff(NvGpuChannel* c) {
  const u64 desde = armGetSystemTick();
  const Result rc = __real_nvGpuChannelKickoff(c);
  Anotar(kKickoff, desde);
  return rc;
}

Result __wrap_nvMapCreate(NvMap* m, void* cpu_addr, u32 size, u32 align, NvKind kind, bool is_cpu_cacheable) {
  const u64 desde = armGetSystemTick();
  const Result rc = __real_nvMapCreate(m, cpu_addr, size, align, kind, is_cpu_cacheable);
  Anotar(is_cpu_cacheable ? kNvMapCacheada : kNvMapSinCache, desde, size);
  return rc;
}

Result __wrap_nvAddressSpaceAllocFixed(NvAddressSpace* a, bool sparse, u64 size, iova_t iova) {
  const u64 desde = armGetSystemTick();
  const Result rc = __real_nvAddressSpaceAllocFixed(a, sparse, size, iova);
  Anotar(kReservaDireccion, desde, size);
  return rc;
}

Result __wrap_nvioctlNvhostAsGpu_MapBufferEx(u32 fd, u32 flags, u32 kind, u32 nvmap_handle, u32 page_size,
                                             u64 buffer_offset, u64 mapping_size, u64 input_offset, u64* offset) {
  const u64 desde = armGetSystemTick();
  const Result rc = __real_nvioctlNvhostAsGpu_MapBufferEx(fd, flags, kind, nvmap_handle, page_size, buffer_offset,
                                                          mapping_size, input_offset, offset);
  Anotar(kMapeoGpu, desde, mapping_size);
  return rc;
}

void __wrap_armDCacheClean(void* addr, size_t size) {
  const u64 desde = armGetSystemTick();
  __real_armDCacheClean(addr, size);
  Anotar(kCacheClean, desde, size);
}

/*
 * The presentation interval (nfsmw_intervalo_swap).
 *
 * It is reapplied on every present, not when the chain is created, on purpose: the WSI sets it to 1
 * when creating the swapchain, and the chain is recreated when switching from docked to handheld.
 * With 0 nothing is touched and the swapchain mode rules (IMMEDIATE -> interval 0).
 *
 * libnx only accepts interval 0 if the NWindow has three or more buffers; the Horizon WSI always
 * configures exactly 3, so the condition holds. 2 is always accepted.
 *
 * Warning: it is 0 and must stay that way. Measured: with the current frame time, setting 2 sends
 * 22 % of the frames to 66.7 ms. It only makes sense with a median frame time below 31 ms.
 */
std::atomic<unsigned> g_intervalo_swap{0};

Result __wrap_nwindowQueueBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence) {
  const u64 desde = armGetSystemTick();
  const unsigned pedido = g_intervalo_swap.load(std::memory_order_relaxed);
  if (pedido != 0 && nw != nullptr && nw->swap_interval != pedido) {
    nwindowSetSwapInterval(nw, pedido);
  }
  const Result rc = __real_nwindowQueueBuffer(nw, slot, fence);
  Anotar(kQueueBuffer, desde);
  return rc;
}

Result __wrap_bqDequeueBuffer(Binder* b, bool async, u32 width, u32 height, s32 format, u32 usage, s32* buf,
                              NvMultiFence* fence) {
  const u64 desde = armGetSystemTick();
  const Result rc = __real_bqDequeueBuffer(b, async, width, height, format, usage, buf, fence);
  Anotar(kDequeueBuffer, desde);
  return rc;
}

void __wrap_svcSleepThread(s64 nano) {
  const u64 desde = armGetSystemTick();
  __real_svcSleepThread(nano);
  Anotar(nano == 0 ? kSleep0 : nano < 0 ? kSleepCeder : nano <= 1000000 ? kSleepCorto : kSleepLargo, desde);
}

/*
 * Counters: 0 = guest present, 1 = read emulated through the shadow,
 * 2 = fault resolved by an SDK handler, 3 = retry emulated through the
 * shadow, 4 = SEH. 28..32 = draws per surface width from IssueDraw
 * (1600 or more, 1280-1599, 640-1279, 256-639, under 256).
 */
// A long frame, in armGetSystemTick ticks (see VentanaTiron).
void RexSwitchPerfTiron(u64 inicio, u64 fin) {
  const u32 i = g_num_tirones.fetch_add(1);
  if (i < kMaxTirones) {
    g_tirones[i] = {inicio, fin};
  }
}

void RexSwitchPerfResolution(unsigned ancho, unsigned alto) {
  g_resolucion.store(((ancho & 0xFFFF) << 16) | (alto & 0xFFFF), std::memory_order_relaxed);
}

// See g_intervalo_swap, next to the nwindowQueueBuffer wrapper. 0 = touch nothing (the normal case).
void RexSwitchPerfIntervaloSwap(unsigned vblanks) {
  g_intervalo_swap.store(vblanks, std::memory_order_relaxed);
}

void RexSwitchPerfCount(unsigned id) {
  if (id < kCounterCount) {
    g_counters[id].fetch_add(1, std::memory_order_relaxed);
  }
  if (id == 0) {
    // Counter 0 is presents. The console overlay clears the "alive" mark and the resolution mark and
    // only waits 100 ms, so it has to be answered on every frame, not once per second. It is writing a
    // few bytes to shared memory.
    const uint32_t res = g_resolucion.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Latir(res >> 16, res & 0xFFFF);
  }
}

/*
 * 5 draws, 6 scissor area of the draws (pixels), 7 queue submissions,
 * 9 resolves, 10 resolve area, 11 render target transfer calls,
 * 12 render targets transferred, 13 transfer draws, 14 textures loaded,
 * 15 shared memory bytes uploaded, 16 pipelines created, 17 physical memory
 * chunks committed on touch, 18 chunks mapped into a view on touch,
 * 21 clipped audio samples, 22 audio buffers with a peak of 0.98 or more.
 */
void RexSwitchPerfAdd(unsigned id, u64 value) {
  if (id < kCounterCount) {
    g_counters[id].fetch_add(value, std::memory_order_relaxed);
  }
}

/* max counter (23 = audio mix peak in ten-thousandths; the report resets it to 0). */
void RexSwitchPerfMax(unsigned id, u64 value) {
  if (id >= kCounterCount) {
    return;
  }
  u64 actual = g_counters[id].load(std::memory_order_relaxed);
  while (value > actual && !g_counters[id].compare_exchange_weak(actual, value, std::memory_order_relaxed)) {
  }
}

/*
 * Bits: 1 draws, 2 transfers, 4 resolves, 8 texture loads,
 * 16 presentation effect, 32 tiled dispatch.
 */
bool RexSwitchPerfSkip(unsigned bit) {
  return (g_skip_mask.load(std::memory_order_relaxed) & bit) != 0;
}

void RexSwitchPerfSetThreadName(u32 handle, const char* name) {
  if (!handle || !name) {
    return;
  }
  for (auto& s : g_slots) {
    if (s.handle.load() == handle) {
      char tmp[sizeof(s.name)]{};
      std::strncpy(tmp, name, sizeof(tmp) - 1);
      std::memcpy(s.name, tmp, sizeof(tmp));
      return;
    }
  }
}

}  // extern "C"
