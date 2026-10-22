// fh2 - the game's memcpy on the host ("copias" means "copies").
//
//   --fh2_memcpy_nativo=false   (the recompiled PowerPC copies, for an A/B test)
//
// The game has two memcpy: sub_824032E0 (443 call sites, with the D3D library) and the C runtime's sub_82C009E0
// (2,453 call sites, dcbt/dcbtst prefetches). Both take r3 = destination, r4 = source, r5 = bytes, copy forward and
// return the destination in r3. Recompiled instruction by instruction they were 4.3 % of the game's main thread on
// the Moto G9 Play (app/out/perfil_moto_anel) and 0.7-1.8 % of its other threads; the host's memcpy does the same
// bytes with NEON and no per-word bookkeeping. A copy is bytes either way, so big-endian needs nothing.
//
// Addresses are translated as REX_LOAD/REX_STORE do (REX_PHYS_HOST_OFFSET: +0x1000 from 0xE0000000). The original
// still runs when a copy crosses 0xE0000000 or the top of the address space, and when the destination starts inside
// the source: there a forward copy repeats the source's first bytes and memmove would not, and some code may count
// on that.

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(fh2_memcpy_nativo, true, "FH2",
                    "The game's memcpy (sub_824032E0, sub_82C009E0) done by the host's memcpy (app/src/fh2_copias.cpp)");

REXCVAR_DEFINE_BOOL(fh2_memcpy_nativo_fisica, false, "FH2",
                    "Also copy on the host when a range touches physical memory (0xA0000000 on, textures and GPU "
                    "data): off, those copies run the game's own memcpy");

REX_EXTERN(__imp__sub_824032E0);
REX_EXTERN(__imp__sub_82C009E0);

namespace {

constexpr uint64_t kPhysical = 0xE0000000u;
constexpr uint64_t kPhysicalViews = 0xA0000000u;

// Copies on the host and returns true, or returns false for the cases the original must do.
bool Copy(uint8_t* base, uint32_t dst, uint32_t src, uint32_t n) {
  if (!REXCVAR_GET(fh2_memcpy_nativo)) return false;
  if (n == 0) return true;
  const uint64_t d = dst, s = src, end_d = d + n, end_s = s + n;
  if (end_d > 0x100000000ull || end_s > 0x100000000ull) return false;
  if ((d < kPhysical && end_d > kPhysical) || (s < kPhysical && end_s > kPhysical)) return false;
  if (d > s && d < end_s) return false;
  if (!REXCVAR_GET(fh2_memcpy_nativo_fisica) && (end_d > kPhysicalViews || end_s > kPhysicalViews)) return false;
  uint8_t* host_d = base + d + (d >= kPhysical ? 0x1000u : 0u);
  const uint8_t* host_s = base + s + (s >= kPhysical ? 0x1000u : 0u);
  // Overlap with the destination before the source: a forward copy, which memmove is.
  if (d < s && end_d > s) {
    std::memmove(host_d, host_s, n);
  } else {
    std::memcpy(host_d, host_s, n);
  }
  return true;
}

}  // namespace

REX_HOOK_RAW(sub_824032E0) {
  if (!Copy(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)) __imp__sub_824032E0(ctx, base);
}

REX_HOOK_RAW(sub_82C009E0) {
  if (!Copy(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)) __imp__sub_82C009E0(ctx, base);
}
