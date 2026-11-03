// fh2 - small guards on game functions ("correções" means "fixes").
//
// Each hook keeps the game's own behaviour and only adds a check the console never needed.

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <atomic>
#include <chrono>

REX_EXTERN(__imp__sub_83201D50);

// sub_83201D50 destroys an object (calls its virtual destructor at +8 and clears its fields) without
// checking for null. Its caller sub_831F9210 takes the object from this+0x98 under a critical section and
// destroys it; the matching creator sub_831F9268 leaves this+0x98 null when the slot pool sub_831F88C8
// hands out nothing (it then returns E_OUTOFMEMORY, 0x8007000E). On Android (S23 Ultra), right after
// SpeechFacade loads, the release ran with this+0x98 still null and the game read guest 0x0000000C and
// closed. Destroying "nothing" does nothing, as delete nullptr does.
REX_HOOK_RAW(sub_83201D50) {
  if (ctx.r3.u32 == 0) {
    static std::atomic<int> reported{0};
    if (reported.fetch_add(1, std::memory_order_relaxed) < 4) {
      REXLOG_WARN("[fh2] sub_83201D50 called with a null object (from {:08X}): skipped",
                  static_cast<uint32_t>(ctx.lr));
    }
    return;
  }
  __imp__sub_83201D50(ctx, base);
}

REX_EXTERN(__imp__sub_82EC93F8);
REX_EXTERN(__imp__sub_82ECEB88);

namespace {

uint32_t CargarU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  __builtin_memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

}  // namespace

// Box-against-box contact (FH2 world props), the rare path of sub_82ED0488 through sub_82ECF9D8. With an S23 save
// from right after the first festival event, two props rest exactly face to face where the player's car loads,
// and the game closed every time it loaded there (on both renderers, and on the PC too).
//
// sub_82EC93F8 walks one face of a convex mesh: r3 = mesh (+84 faces, 12 bytes each; +98 the number of faces),
// r4 = face, taken by sub_82ED0488 from the last word of a closest-point record. On this path the closest-point
// routine never writes that word: it keeps a float (2E0DAEE0), so the "face" is 224 on a box of 10. The read runs
// past the mesh into whatever follows it; on the console that is the next prop's mesh (they are laid out
// 0x3D0 apart) and the garbage there happens to be harmless, here it is a null pointer and the game reads guest 0.
// Keep the face inside the mesh.
namespace {
// What the last GJK test on this thread answered (see sub_82ECEB88 below): 0 apart, 1 intersect, 2 apart at
// distance zero turned into intersect by the guard. And the first mesh of the pair sub_82ED0488 is walking.
thread_local uint32_t g_gjk_ruta = 0;
thread_local uint32_t g_malla_a = 0;
constexpr uint32_t kCaraA = 0x82ED094C;  // return of the face walk of shape A (record word at r1+396)
constexpr uint32_t kCaraB = 0x82ED097C;  // return of the face walk of shape B (record word at r1+412)
}  // namespace

REX_HOOK_RAW(sub_82EC93F8) {
  const uint32_t malla = ctx.r3.u32;
  const uint32_t cara = ctx.r4.u32 & 0xFF;
  const uint32_t caras = malla >= 0x10000 ? base[malla + 98] : 0;
  // Both faces of a pair, logged when either is outside its mesh: is it the other mesh's index?
  const uint32_t sitio = static_cast<uint32_t>(ctx.lr);
  if (sitio == kCaraA) {
    g_malla_a = malla;
  } else if (sitio == kCaraB && g_malla_a >= 0x10000 && malla >= 0x10000) {
    const uint32_t palabra_a = CargarU32(base, ctx.r1.u32 + 396), palabra_b = CargarU32(base, ctx.r1.u32 + 412);
    const uint32_t cara_a = palabra_a & 0xFF, caras_a = base[g_malla_a + 98];
    const uint32_t cara_b = palabra_b & 0xFF, caras_b = caras;
    if (cara_a >= caras_a || cara_b >= caras_b) {
      static std::atomic<int> pares{0};
      if (pares.fetch_add(1, std::memory_order_relaxed) < 300) {
        static const char* const kRutas[] = {"separadas", "penetrando", "forzada"};
        REXLOG_WARN("[fh2] par fuera de malla: A cara {} ({:08X}) de {} ({:08X}) | B cara {} ({:08X}) de {} ({:08X}) | gjk {}",
                    cara_a, palabra_a, caras_a, g_malla_a, cara_b, palabra_b, caras_b, malla,
                    kRutas[g_gjk_ruta < 3 ? g_gjk_ruta : 0]);
      }
    }
  }
  if (caras != 0 && cara >= caras) {
    // Counted, not only the first few: to see whether these line up with the cars thrown in collisions. One line
    // per second at most (with the raw word, the mesh and the caller), and a total every 10 s.
    using Reloj = std::chrono::steady_clock;
    static std::atomic<uint64_t> total{0}, desde_informe{0};
    static std::atomic<int64_t> ultimo_linea{0}, ultimo_informe{0};
    const int64_t ahora =
        std::chrono::duration_cast<std::chrono::milliseconds>(Reloj::now().time_since_epoch()).count();
    total.fetch_add(1, std::memory_order_relaxed);
    const uint64_t n = desde_informe.fetch_add(1, std::memory_order_relaxed) + 1;
    int64_t previo = ultimo_linea.load(std::memory_order_relaxed);
    if (ahora - previo >= 1000 && ultimo_linea.compare_exchange_strong(previo, ahora)) {
      REXLOG_WARN("[fh2] sub_82EC93F8 (desde {:08X}): cara {} (palabra {:08X}) de una malla de {} ({:08X}): se usa la {}",
                  static_cast<uint32_t>(ctx.lr), cara, ctx.r4.u32, caras, malla, cara % caras);
    }
    previo = ultimo_informe.load(std::memory_order_relaxed);
    if (previo == 0) {
      ultimo_informe.compare_exchange_strong(previo, ahora);
    } else if (ahora - previo >= 10000 && ultimo_informe.compare_exchange_strong(previo, ahora)) {
      desde_informe.store(0, std::memory_order_relaxed);
      REXLOG_WARN("[fh2] caras fuera de malla: {} en 10 s ({} en total)", n, total.load());
    }
    ctx.r4.u64 = cara % caras;
  }
  __imp__sub_82EC93F8(ctx, base);
}

// sub_82ECEB88 is the GJK test between two convex shapes: it returns (r3, low byte) whether they intersect and
// writes the two closest points to r4 and r5. When it says "apart" but the two points are the same (the props
// above, face to face), its caller sub_82ECF9D8 normalises the zero vector between them (vrsqrtefp of 0 is
// infinity, 0 * infinity is NaN) and the NaN normal runs on into the contact. Shapes at distance zero touch:
// answer "intersect", so the caller takes its penetration path (sub_82ECD640, sub_82ECF738), made for contact.
REX_HOOK_RAW(sub_82ECEB88) {
  const uint32_t punto_a = ctx.r4.u32, punto_b = ctx.r5.u32;
  __imp__sub_82ECEB88(ctx, base);
  g_gjk_ruta = (ctx.r3.u32 & 0xFF) != 0 ? 1 : 0;
  if ((ctx.r3.u32 & 0xFF) != 0 || punto_a < 0x10000 || punto_b < 0x10000) {
    return;
  }
  for (uint32_t o = 0; o < 12; o += 4) {
    if (CargarU32(base, punto_a + o) != CargarU32(base, punto_b + o)) {
      return;
    }
  }
  g_gjk_ruta = 2;
  static std::atomic<int> reported{0};
  if (reported.fetch_add(1, std::memory_order_relaxed) < 4) {
    REXLOG_WARN("[fh2] sub_82ECEB88 (desde {:08X}): formas a distancia cero tratadas como en contacto",
                static_cast<uint32_t>(ctx.lr));
  }
  ctx.r3.u64 = 1;
}
