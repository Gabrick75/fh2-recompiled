// Forza Horizon 2's hooks for the native renderer (src/nativo, built with FH2_RECOMP). Addresses:
// docs/renderizador_nativo.md.
//
// - The shader constructors tell the renderer which library container each D3D shader object comes from.
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>

#include "nfsmw_nativo_ganchos.h"

namespace nativo = nfsmw::nativo;

// CreatePixelShader / CreateVertexShader (r3 = 2008 container, return the object in r3).
REX_EXTERN(__imp__sub_82B70280);
REX_HOOK_RAW(sub_82B70280) {
  const auto* entrada = nativo::ganchos_detalle::IdentificarCreacion(base, ctx.r3.u32, false);
  __imp__sub_82B70280(ctx, base);
  nativo::ganchos_detalle::RecordarCreacion(ctx.r3.u32, entrada, false);
}

REX_EXTERN(__imp__sub_82B70468);
REX_HOOK_RAW(sub_82B70468) {
  const auto* entrada = nativo::ganchos_detalle::IdentificarCreacion(base, ctx.r3.u32, true);
  const uint32_t contenedor = ctx.r3.u32;
  __imp__sub_82B70468(ctx, base);
  nativo::ganchos_detalle::RecordarCreacion(ctx.r3.u32, entrada, true);
  // Diagnostics: the object's first words (where its microcode lives).
  static int avisos = 0;
  if (avisos < 12 && ctx.r3.u32) {
    ++avisos;
    const auto* w = reinterpret_cast<const uint32_t*>(base + ctx.r3.u32);
    REXLOG_INFO("[fh2 nativo] VS {:08X} (contenedor {:08X}): {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
                "{:08X} {:08X}", ctx.r3.u32, contenedor, __builtin_bswap32(w[0]), __builtin_bswap32(w[1]),
                __builtin_bswap32(w[2]), __builtin_bswap32(w[3]), __builtin_bswap32(w[4]), __builtin_bswap32(w[5]),
                __builtin_bswap32(w[6]), __builtin_bswap32(w[7]), __builtin_bswap32(w[8]), __builtin_bswap32(w[9]));
  }
}

// The vertex fetch patcher (sub_8241ACA0: r3 = VS object, r4 = destination, r7 = variant; the object keeps its
// patched variants 416 bytes apart, as in NFS Carbon). Each copy it writes is reported, so the ring can tell which
// VS an IM_LOAD of that copy is (Fh2VsPorDireccion).
REX_EXTERN(__imp__sub_8241ACA0);
REX_HOOK_RAW(sub_8241ACA0) {
  const uint32_t objeto = ctx.r3.u32, destino = ctx.r4.u32;
  __imp__sub_8241ACA0(ctx, base);
  nativo::Fh2AnotarParcheVs(objeto, destino);
}
