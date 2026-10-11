// fh2 - small guards on game functions ("correções" means "fixes").
//
// Each hook keeps the game's own behaviour and only adds a check the console never needed.

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <atomic>

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
REX_HOOK_RAW(sub_82EC93F8) {
  const uint32_t malla = ctx.r3.u32;
  const uint32_t cara = ctx.r4.u32 & 0xFF;
  const uint32_t caras = malla >= 0x10000 ? base[malla + 98] : 0;
  if (caras != 0 && cara >= caras) {
    static std::atomic<int> reported{0};
    if (reported.fetch_add(1, std::memory_order_relaxed) < 4) {
      REXLOG_WARN("[fh2] sub_82EC93F8 (desde {:08X}): cara {} de una malla de {} ({:08X}): se usa la {}",
                  static_cast<uint32_t>(ctx.lr), cara, caras, malla, cara % caras);
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
