// fh2 - game functions replaced for speed ("desempenho" means "performance").
//
// WAITING FOR ROOM IN THE COMMAND RING (sub_82B72A90)
//
// When the GPU command ring is full, the game's D3D waits for the command processor (CP) to move its read
// pointer: it SPINS, re-reading the read pointer copy in guest memory ([device+0x2B10]) and calling
// sub_82B72A90 on each turn (a short spin of priority hints, then the hang watchdog; it returns 0 when the
// pointer has not moved for too long). On the Xbox 360 that was cheap. Here the CP is the bottleneck, so the
// game's render thread spun the whole frame at 100 % of a core (simpleperf on the S23 Ultra, event menu:
// sub_823F94D0 + sub_82B72A90 were 15 % of all the app's samples), heating the phone into thermal throttling,
// which slows the CP and the GPU in turn.
//
// The hook: when the pointer has not moved since the last turn, sleep on a futex until the CP publishes a new
// one (rex_cp_rptr_seq, src/graphics/command_processor.cpp), at most 1 ms. The game learns of the progress as
// soon as before, without burning a core. Ported from NFSMW-Recompiled-Mobile (android/app/src/main/cpp/
// ganchos.cpp, where the same loop is sub_82597690 -> sub_825A5D18).
//
// Only for the four ring-room loops, told apart by the return address; the other callers of sub_82B72A90 wait
// for fences the CP writes mid-buffer, and sleeping there would add latency. Cvar fh2_espera_anel turns it off
// to compare.

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(fh2_espera_anel, true, "FH2",
                    "Sleep instead of spinning while the game waits for room in the GPU command ring "
                    "(app/src/fh2_desempenho.cpp)");

REX_EXTERN(__imp__sub_82B72A90);

namespace {

// Return addresses of the calls from the four loops that wait for the ring's read pointer to move.
constexpr uint32_t kRingWaitReturns[] = {0x823F9568, 0x82419164, 0x8241C9F8, 0x82B6EC3C};

// Inside the D3D device: pointer to the guest copy of the CP read pointer (lwz rX, 0x2B10(device)).
constexpr uint32_t kDeviceReadPointerCopy = 0x2B10;

uint32_t LoadGuestU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  __builtin_memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

struct ReadPointerSignal {
  std::atomic<uint32_t>* sequence = nullptr;
  std::atomic<uint32_t>* waiters = nullptr;
};

}  // namespace

// The native renderer's ring (app/src/nativo/nfsmw_espera_anillo.cpp) gives the same signal.
extern std::atomic<uint32_t> g_fh2_anillo_seq;
extern std::atomic<uint32_t> g_fh2_anillo_esperando;

namespace {

// The GPU plugin's counters, looked up once it is loaded; or the native renderer's ring, once it has returned a read
// pointer (its sequence has moved).
const ReadPointerSignal* Signal() {
  static ReadPointerSignal signal;
  static std::atomic<bool> ready{false};
  if (!ready.load(std::memory_order_acquire)) {
    if (g_fh2_anillo_seq.load(std::memory_order_relaxed) != 0) {
      signal.sequence = &g_fh2_anillo_seq;
      signal.waiters = &g_fh2_anillo_esperando;
      REXLOG_INFO("[fh2] espera do anel: dorme no futex do anel nativo");
      ready.store(true, std::memory_order_release);
      return &signal;
    }
    // dlopen and dlsym walk the dynamic loader's tables: not on every turn of the game's spin (in native mode the
    // plugin is never loaded, and that lookup alone was a third of the render thread's samples on the PC).
    static uint32_t intentos = 0;
    if ((intentos++ & 4095) != 0) {
      return nullptr;
    }
    void* plugin = dlopen("librexgpu-xenos.so", RTLD_NOW | RTLD_NOLOAD);
    if (!plugin) {
      return nullptr;
    }
    signal.sequence = static_cast<std::atomic<uint32_t>*>(dlsym(plugin, "rex_cp_rptr_seq"));
    signal.waiters = static_cast<std::atomic<uint32_t>*>(dlsym(plugin, "rex_cp_rptr_waiters"));
    REXLOG_INFO("[fh2] espera do anel: {}", signal.sequence && signal.waiters
                                                 ? "dorme no futex do processador de comandos"
                                                 : "contador do plugin ausente, gira como no console");
    ready.store(true, std::memory_order_release);
  }
  return signal.sequence && signal.waiters ? &signal : nullptr;
}

bool IsRingWait(uint32_t return_address) {
  for (uint32_t address : kRingWaitReturns) {
    if (return_address == address) {
      return true;
    }
  }
  return false;
}

}  // namespace

REX_HOOK_RAW(sub_82B72A90) {
#if defined(__linux__)
  if (REXCVAR_GET(fh2_espera_anel) && IsRingWait(static_cast<uint32_t>(ctx.lr))) {
    if (const ReadPointerSignal* signal = Signal()) {
      // Sequence first, pointer second: if the CP publishes in between, the sequence has already changed and
      // the futex returns at once.
      signal->waiters->fetch_add(1, std::memory_order_seq_cst);
      const uint32_t before = signal->sequence->load(std::memory_order_seq_cst);
      // r3 points to the wait state: { device, ..., read pointer seen at +8 }.
      const uint32_t state = ctx.r3.u32;
      const uint32_t device = LoadGuestU32(base, state + 0);
      const uint32_t seen = LoadGuestU32(base, state + 8);
      const uint32_t read_pointer = LoadGuestU32(base, LoadGuestU32(base, device + kDeviceReadPointerCopy));
      if (read_pointer == seen) {
        timespec limit{0, 1000000};  // 1 ms
        syscall(SYS_futex, signal->sequence, FUTEX_WAIT_PRIVATE, before, &limit, nullptr, 0);
      }
      signal->waiters->fetch_sub(1, std::memory_order_seq_cst);
    }
  }
#endif
  __imp__sub_82B72A90(ctx, base);
}

// ONE TILE INSTEAD OF THREE (predicated tiling played back in one pass)
//
// With MSAA 4x the 1280x720 scene does not fit in the Xbox 360's 10 MB of EDRAM, so the game draws it in three
// 1280x256 tiles (predicated tiling): the game records the scene once, marking each draw with the tiles it touches
// (SET_BIN_MASK), and the D3D plays the recording back once per tile (SET_BIN_SELECT 0x3, 0xC, 0x30), resolving
// each tile into its rows of the scene texture. Here the GPU command processor (CP) is the bottleneck, so that is
// three times its work for one picture. With the MSAA cap at one sample (gpu_sin_msaa) the whole scene fits
// (1280x720 x 4 bytes, color and depth, 7.4 MB), so the playback runs once over the union of the tiles, and the CP
// ignores the predication (gpu_ignorar_predicacao): every draw marked for any tile runs in that single pass. Idea
// from NFSMW-Recompiled-Mobile (ganchos.cpp, nfsmw_una_franja), which hooks its game's BeginTiling.
//
// The game keeps culling and marking for three tiles; only the D3D is handed one tile, for the length of each call.
//
// sub_8247A868 is the BeginTiling of the playback (called by sub_8247C2F8): r5 tile count, r6 tile rects
// { x1, y1, x2, y2 }, both from a global table of tiling setups at 0x833F9380 + index * 0x4D0 (count at +0x10,
// rects at +0x20). The hook hands it one tile, the union, through that table entry, and restores the entry right
// after the call.
// The window offset and scissor the game records per tile, predicated, and its per-tile resolves are handled in
// the command processor (src/graphics/command_processor.cpp, gpu_ignorar_predicacao). PC, in race: ~3100-4200
// -> ~1900 draws per frame, 19 -> 22 fps; picture identical.
REXCVAR_DEFINE_BOOL(fh2_uma_faixa, true, "FH2",
                    "Play the scene back in one tile instead of three when the MSAA cap leaves one sample "
                    "(gpu_sin_msaa=true or gpu_msaa_muestras=1); app/src/fh2_desempenho.cpp");

namespace {

void StoreGuestU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  __builtin_memcpy(base + address, &value, sizeof(value));
}

// The last tiling merged: its first tile as the game set it, and the union that replaced it (see the Clear hook).
std::atomic<uint64_t> g_first_tile_xy2{0};  // first tile's x2 << 32 | y2 (x1 = y1 = 0 in this game)
std::atomic<uint64_t> g_union_xy2{0};       // union's x2 << 32 | y2

// Whether one tile is in use: the MSAA cap (a GPU plugin cvar) leaves one sample, the only case in which the
// whole scene fits in the EDRAM. The first time it is, the CP is told to ignore the predication.
bool OneTile() {
  static const bool one = [] {
    // The native renderer (nfsmw_renderizador = nativo) draws MSAA targets with one sample and its render targets
    // have no EDRAM limit: one tile always fits, as NFSMW's nfsmw_render_sin_mosaico does for that game.
    const bool one_sample = rex::cvar::GetFlagByName("gpu_sin_msaa") == "true" ||
                            rex::cvar::GetFlagByName("gpu_msaa_muestras") == "1" ||
                            rex::cvar::GetFlagByName("nfsmw_renderizador") == "nativo";
    const bool result = REXCVAR_GET(fh2_uma_faixa) && one_sample;
    if (result) {
      rex::cvar::SetFlagByName("gpu_ignorar_predicacao", "true");
    }
    REXLOG_INFO("[fh2] uma faixa: {}", !REXCVAR_GET(fh2_uma_faixa) ? "desligada (fh2_uma_faixa=false)"
                                       : result ? "a cena em uma passada em vez de três"
                                                : "inativa, o MSAA não cabe na EDRAM em uma faixa");
    return result;
  }();
  return one;
}

}  // namespace

// The native renderer's ring asks whether the scene is played back in one tile (src/nativo, FH2_RECOMP): then it
// treats predicated packets as gpu_ignorar_predicacao does in the emulation.
bool Fh2UmaFaixaAtiva() { return OneTile(); }

namespace {

constexpr uint32_t kTilingSetupCount = 0x10;
constexpr uint32_t kTilingSetupRects = 0x20;

// Rewrites the first of 'count' tile rects at 'rects' to their union for the duration of 'call', then restores it.
// Notes the original first tile and the union for the Clear hook.
template <typename Call>
void WithOneTile(uint8_t* base, uint32_t rects, uint32_t count, Call call) {
  uint32_t saved[4];
  uint32_t x1 = UINT32_MAX, y1 = UINT32_MAX, x2 = 0, y2 = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t r = rects + 16 * i;
    x1 = std::min(x1, LoadGuestU32(base, r));
    y1 = std::min(y1, LoadGuestU32(base, r + 4));
    x2 = std::max(x2, LoadGuestU32(base, r + 8));
    y2 = std::max(y2, LoadGuestU32(base, r + 12));
  }
  for (uint32_t i = 0; i < 4; ++i) {
    saved[i] = LoadGuestU32(base, rects + 4 * i);
  }
  if (saved[0] == 0 && saved[1] == 0 && x1 == 0 && y1 == 0) {
    g_first_tile_xy2.store(uint64_t(saved[2]) << 32 | saved[3], std::memory_order_relaxed);
    g_union_xy2.store(uint64_t(x2) << 32 | y2, std::memory_order_relaxed);
  }
  StoreGuestU32(base, rects, x1);
  StoreGuestU32(base, rects + 4, y1);
  StoreGuestU32(base, rects + 8, x2);
  StoreGuestU32(base, rects + 12, y2);
  call();
  for (uint32_t i = 0; i < 4; ++i) {
    StoreGuestU32(base, rects + 4 * i, saved[i]);
  }
}

}  // namespace

// The recording side (sub_82478390: r9 tile count, r8 tile rects, from the game's renderer structure at +0x60,
// called from 0x82F66F50): the D3D gets one tile only for the call, so the window scissor and viewport it records
// cover the whole scene; the game's own structure is left as it was (rewriting it made the game cull most of the
// world, which it does per tile).
REX_EXTERN(__imp__sub_82478390);

REX_HOOK_RAW(sub_82478390) {
  const uint32_t count = ctx.r9.u32;
  if (count < 2 || count > 15 || !OneTile()) {
    __imp__sub_82478390(ctx, base);
    return;
  }
  ctx.r9.u64 = 1;
  WithOneTile(base, ctx.r8.u32, count, [&] { __imp__sub_82478390(ctx, base); });
}

REX_EXTERN(__imp__sub_8247A868);

REXCVAR_DEFINE_INT32(fh2_teste_r4_reproducao, -1, "FH2", "Debug: force r4 of sub_8247A868 (-1 = as is)");

REX_HOOK_RAW(sub_8247A868) {
  if (REXCVAR_GET(fh2_teste_r4_reproducao) >= 0) {
    ctx.r4.u64 = uint64_t(REXCVAR_GET(fh2_teste_r4_reproducao));
  }
  const uint32_t count = ctx.r5.u32;
  if (count < 2 || count > 15 || !OneTile()) {
    __imp__sub_8247A868(ctx, base);
    return;
  }
  const uint32_t count_field = ctx.r6.u32 - kTilingSetupRects + kTilingSetupCount;
  const uint32_t saved_count = LoadGuestU32(base, count_field);
  StoreGuestU32(base, count_field, 1);
  ctx.r5.u64 = 1;
  WithOneTile(base, ctx.r6.u32, count, [&] { __imp__sub_8247A868(ctx, base); });
  StoreGuestU32(base, count_field, saved_count);
}

// The first-tile clear (sub_82417A20, the D3D Clear: r6..r9 the rect x1, y1, x2, y2). With tiling the D3D clears
// only the first tile's area from sub_8241DF18 (0x8241E530): the other tiles were cleared by the resolve that ends
// the tile before them. In one tile that left everything under the first 256 rows uncleared (stale EDRAM: a
// blown-out band and black below). So while tiles are merged, a clear of exactly the original first tile covers
// the union.
REX_EXTERN(__imp__sub_82417A20);

REX_HOOK_RAW(sub_82417A20) {
  if (ctx.r6.u32 == 0 && ctx.r7.u32 == 0) {
    const uint64_t first = g_first_tile_xy2.load(std::memory_order_relaxed);
    const uint64_t merged = g_union_xy2.load(std::memory_order_relaxed);
    if (first && merged && first != merged && (uint64_t(ctx.r8.u32) << 32 | ctx.r9.u32) == first) {
      ctx.r8.u64 = merged >> 32;
      ctx.r9.u64 = merged & 0xFFFFFFFFu;
    }
  }
  __imp__sub_82417A20(ctx, base);
}

// RENDER MODE (investigation): the game's table of render setups at 0x833E9380 (0x4D0 bytes each: name, width,
// height, MSAA level, tile count, tile rects): 0 "1280x720 4xMSAA Horizontal Split" (the default), 1 "1280x720
// 2xMSAA Horizontal Split", 2 "1280x720 1xMSAA No Split", 3 "Largest 2xMSAA No Split" (1120x584). The index is at
// +0x2438 of the structure sub_8247C2F8 gets in r3. fh2_modo_render >= 0 forces it.
//
// STATUS: forcing 2 (no MSAA, one tile: CPU of the command processor 24 ms per frame against 34 with fh2_uma_faixa)
// or 3 (1120x584, a lower render resolution) draws the world's static geometry (road, ground, walls) black: in
// those setups the scene's depth sits at EDRAM base 720 instead of 1024, over the screen-space shadow mask the game
// drew there just before. Unsolved; off by default.
REXCVAR_DEFINE_INT32(fh2_modo_render, -1, "FH2",
                     "Debug: force the game's render setup (0 4xMSAA 3 tiles, 1 2xMSAA 2 tiles, 2 no MSAA, 3 "
                     "1120x584 2xMSAA); -1 = the game's choice");
// The scene's setup index comes from sub_82482390 (called at 0x8246AE54 on the settings object at [0x83422068]); the
// game also switches to setup 2 for other passes and back. The forced value replaces that return value.
REX_EXTERN(__imp__sub_82482390);
REX_HOOK_RAW(sub_82482390) {
  __imp__sub_82482390(ctx, base);
  const int32_t forced = REXCVAR_GET(fh2_modo_render);
  if (forced >= 0 && forced <= 3) {
    ctx.r3.u64 = uint64_t(forced);
  }
}

// REFLECTION CUBE FACES PER FRAME (sub_824041C8)
//
// The game's render scenarios (media/renderscenarios.zip, CDynamicRender*Control, same system as Forza Horizon 1)
// have an EnvMapFrequencyScale control, id 30 (+364 in a scenario, offset table at 0x8333F928). sub_824041C8 reads
// its value (a debug override, then the scenario stack, then the default 1.0), and sub_82409D08 turns it into the
// number of faces of the car reflection cube redrawn this frame: int(value * 6) clamped to 1-6, unless it forces 6
// (+457 / +480 of its argument) or 1 (some camera modes). No FH2 scenario sets the control, so the game redraws all
// six faces, and their mip chains, every frame. Pinyon Shift measured the same control at a quarter rate on FH1
// (Odin 2, Adreno 740: 25.0 -> 22.7 ms a frame, reflections looking the same).
//
// With fh2_reflexo_faces at 1-5 the value read by sub_82409D08 (told apart by the return address) is capped so that
// it gives that many faces: the cube is still fully redrawn, over 6 / faces frames. The forced cases and the other
// readers of the value (which only compare it with 0) are left as they are.
REXCVAR_DEFINE_INT32(fh2_reflexo_faces, 6, "FH2",
                     "Faces of the car reflection cube redrawn per frame, 1-6 (6: the game's own, all of them every "
                     "frame; app/src/fh2_desempenho.cpp)");

// A/B test within one game (same heat, same place): with fh2_reflexo_alternar_s > 0 the faces alternate every that
// many seconds between fh2_reflexo_faces (2 when it is 6) and the game's 6, starting with the reduced ones. Each switch
// is logged ("[fh2] reflejo A/B"), to be matched with the _uso.log samples and the C2 lines by their time.
REXCVAR_DEFINE_INT32(fh2_reflexo_alternar_s, 0, "FH2",
                     "Test: alternate the reflection cube faces per frame between fh2_reflexo_faces and 6 every this "
                     "many seconds (0: off)");

REX_EXTERN(__imp__sub_824041C8);

namespace {

// Return address of the call to sub_824041C8 in sub_82409D08 (the faces per frame).
constexpr uint32_t kReflectionFacesReturn = 0x82409DE0;

// The faces for this frame: the cvar, or the A/B alternation.
int32_t ReflectionFaces() {
  int32_t faces = REXCVAR_GET(fh2_reflexo_faces);
  const int32_t period = REXCVAR_GET(fh2_reflexo_alternar_s);
  if (period <= 0) {
    return faces;
  }
  using Clock = std::chrono::steady_clock;
  static const Clock::time_point start = Clock::now();
  static int64_t last_window = -1;
  const int64_t window = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - start).count() / period;
  const int32_t reduced = faces >= 1 && faces < 6 ? faces : 2;
  faces = window % 2 == 0 ? reduced : 6;
  if (window != last_window) {
    last_window = window;
    REXLOG_INFO("[fh2] reflejo A/B: ventana {} de {} s, {} caras por fotograma", window, period, faces);
  }
  return faces;
}

}  // namespace

REX_HOOK_RAW(sub_824041C8) {
  const uint32_t from = static_cast<uint32_t>(ctx.lr);
  __imp__sub_824041C8(ctx, base);
  if (from != kReflectionFacesReturn) {
    return;
  }
  const int32_t faces = ReflectionFaces();
  if (faces < 1 || faces >= 6) {
    return;
  }
  // fctiwz truncates: half a face of margin keeps faces * 6 / 6 from landing just under the integer.
  const double limit = (faces + 0.5) / 6.0;
  if (ctx.f1.f64 > limit) {
    static std::atomic<int> reported{0};
    if (reported.fetch_add(1, std::memory_order_relaxed) == 0) {
      REXLOG_INFO("[fh2] reflejo: EnvMapFrequencyScale {:.2f} -> {:.2f} ({} caras por fotograma)", ctx.f1.f64, limit,
                  faces);
    }
    ctx.f1.f64 = limit;
  }
}
