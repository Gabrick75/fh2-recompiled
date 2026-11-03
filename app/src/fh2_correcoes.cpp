// fh2 - small guards on game functions ("correções" means "fixes").
//
// Each hook keeps the game's own behaviour and only adds a check the console never needed.

#include <rex/cvar.h>
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

// Convex-against-convex contact (car hulls against each other, world props), sub_82ED0488 through sub_82ECF9D8.
//
// sub_82EC93F8(r3 = hull, r4 = feature) takes the feature from the last word of a closest-point record. It reads
// the face record at hull+84 + feature*12 (lists at +0/+4, their lengths at +8/+9), and its vertex branch
// (loc_82EC9618) takes the contact point from the vertex at hull+80 + feature*16. So the feature is not always a
// face: a car hull of 18-20 faces gets 18-23 here in every car-on-car hit, and the console reads the face record
// past the end of the array just the same (whatever follows it in the hull's data) and uses that vertex.
//
// The first guard (2026-10-11, two festival props loading face to face) kept the feature below the face count
// (+98). That moved the contact to another vertex of the hull on every one of those hits, and the cars were
// thrown into the air or sideways (2026-11-02: 72 remaps in a 74 s race, each burst on a car-on-car contact in
// the captures). Now the feature is left alone, as on the console, unless the record it reads has a list
// pointing at guest page zero: that is the festival props' crash (their records ran into a null pointer).
REXCVAR_DEFINE_BOOL(fh2_colisao_cara_mod, false, "FH2",
                    "Old guard of the convex contact: keep the feature index below the hull's face count. It moves "
                    "car-on-car contacts to another vertex and throws the cars (app/src/fh2_correcoes.cpp)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_HOOK_RAW(sub_82EC93F8) {
  const uint32_t malla = ctx.r3.u32;
  const uint32_t cara = ctx.r4.u32 & 0xFF;
  const uint32_t caras = malla >= 0x10000 ? base[malla + 98] : 0;
  if (caras != 0 && cara >= caras) {
    static std::atomic<uint64_t> fuera{0}, corregidas{0};
    fuera.fetch_add(1, std::memory_order_relaxed);
    bool corregir = REXCVAR_GET(fh2_colisao_cara_mod);
    if (!corregir) {
      // The record the game is about to read, past the face array.
      const uint32_t registro = CargarU32(base, malla + 84) + cara * 12;
      const uint32_t lista_0 = CargarU32(base, registro + 0), lista_4 = CargarU32(base, registro + 4);
      const uint8_t largo_8 = base[registro + 8], largo_9 = base[registro + 9];
      corregir = (largo_9 != 0 && lista_4 < 0x10000) || (largo_8 != 0 && lista_0 < 0x10000);
    }
    if (corregir) {
      const uint64_t n = corregidas.fetch_add(1, std::memory_order_relaxed);
      if (n < 8) {
        REXLOG_WARN("[fh2] sub_82EC93F8 (desde {:08X}): rasgo {} de una malla de {} caras ({:08X}): se usa la cara {}",
                    static_cast<uint32_t>(ctx.lr), cara, caras, malla, cara % caras);
      }
      ctx.r4.u64 = cara % caras;
    }
    using Reloj = std::chrono::steady_clock;
    static std::atomic<int64_t> ultimo_informe{0};
    const int64_t ahora =
        std::chrono::duration_cast<std::chrono::milliseconds>(Reloj::now().time_since_epoch()).count();
    int64_t previo = ultimo_informe.load(std::memory_order_relaxed);
    if (previo == 0) {
      ultimo_informe.compare_exchange_strong(previo, ahora);
    } else if (ahora - previo >= 10000 && ultimo_informe.compare_exchange_strong(previo, ahora)) {
      REXLOG_INFO("[fh2] rasgos mas alla de las caras: {} en total, {} corregidos ({})", fuera.load(),
                  corregidas.load(), REXCVAR_GET(fh2_colisao_cara_mod) ? "guarda antigua" : "solo punteros nulos");
    }
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
  if ((ctx.r3.u32 & 0xFF) != 0 || punto_a < 0x10000 || punto_b < 0x10000) {
    return;
  }
  for (uint32_t o = 0; o < 12; o += 4) {
    if (CargarU32(base, punto_a + o) != CargarU32(base, punto_b + o)) {
      return;
    }
  }
  static std::atomic<int> reported{0};
  if (reported.fetch_add(1, std::memory_order_relaxed) < 4) {
    REXLOG_WARN("[fh2] sub_82ECEB88 (desde {:08X}): formas a distancia cero tratadas como en contacto",
                static_cast<uint32_t>(ctx.lr));
  }
  ctx.r3.u64 = 1;
}
