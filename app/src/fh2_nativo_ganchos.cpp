// Forza Horizon 2's hooks for the native renderer (src/nativo, built with FH2_RECOMP). Addresses:
// docs/renderizador_nativo.md.
//
// - The shader constructors tell the renderer which library container each D3D shader object comes from.
#include <cstdint>

#include <rex/hook.h>

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
  __imp__sub_82B70468(ctx, base);
  nativo::ganchos_detalle::RecordarCreacion(ctx.r3.u32, entrada, true);
}
