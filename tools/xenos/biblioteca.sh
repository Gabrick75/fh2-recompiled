#!/bin/bash
# Builds FH2's translated shader library for the native renderer ("biblioteca" means "library") and measures how many
# of the shaders the game really loaded (the emulator's shader storage, .xsh) it covers:
#   1. unpacks the track .fxobj from the zips and extracts every shader container;
#   2. translates them to HLSL with XenosRecomp as changed by nfsmw-nx and this project (tools/xenos/recomp);
#   3. compiles to SPIR-V with DXC, strips the debug names (spirv-opt) and validates with spirv-val;
#   4. packs the library the native renderer loads (fh2_shaders.nfsp, also copied next to the PC build);
#   5. coverage (cobertura.py).
# Output in app/out/shaders_fh2 (made from the game's data: never commit or share it).
#
#   tools/xenos/biblioteca.sh
#
# O toolchain é TRAVADO e baixado sozinho (tools/xenos/ferramentas.sh): DXC v1.9.2607
# do GitHub e SPIR-V do NDK 29.0.14206865 via sdkmanager. Nada manual, igual em todo PC.
set -euo pipefail

RAIZ=$(cd "$(dirname "$0")/../.." && pwd)
REC=$RAIZ/tools/xenos/recomp
R=$REC
S=$RAIZ/app/out/shaders_fh2; B=$RAIZ/app/out/xenos_bin
TP=$RAIZ/sdk/thirdparty; M=$TP/libmspack/libmspack/mspack

mkdir -p "$B"

# ---- Versões das ferramentas (reportadas; salvas em manifesto após limpar $S) ----
reportar_versao() {
  local nome="$1"; shift
  local vers
  vers=$("$@" --version 2>/dev/null | head -1 || echo "(não encontrado)")
  echo "[versao] $nome: $vers ($1)"
}

# Toolchain exato, com download/instalação automática (nada manual)
source "$RAIZ/tools/xenos/ferramentas.sh"
ferramentas_resolver || exit 1

XSH=${XSH:-$RAIZ/app/out/run/user/cache/shaders/shareable/4D530AA4.xsh}
COMUN=${COMUN:-$R/XenosRecomp/shader_common.h}

# ---- Imagens do executável (obrigatórias para containers da D3D library) ----
IMAGENS=($RAIZ/app/out/shaders_fh2_imagem.*)
if [ ! -e "${IMAGENS[0]}" ]; then
  echo "ERRO: imagens do executável não encontradas em $RAIZ/app/out/shaders_fh2_imagem.*" >&2
  echo "      Rode antes: tools/rodar_pc.sh --xex_despejar_imagem=$RAIZ/app/out/shaders_fh2_imagem" >&2
  exit 1
fi
echo "[info] imagens do executável: ${IMAGENS[*]}"

# ---- Compila ferramentas auxiliares (versões fixas para reprodutibilidade) ----
CC=${CC:-gcc}
CXX=${CXX:-g++}
[ -x "$B/lzx_xmem" ] || "$CC" -O1 -I"$M" "$RAIZ/tools/xenos/lzx_xmem.c" "$M/lzxd.c" "$M/system.c" -o "$B/lzx_xmem" || exit 1
"$CXX" -std=c++23 -O1 -I"$R" -I"$R/XenosRecomp" -I"$TP/fmt/include" -I"$TP/xxHash" -include "$R/pch_min.h" -DFMT_HEADER_ONLY \
  -DXXH_INLINE_ALL -DNFSMW_RECOMP -DFH2_RECOMP "$R/nfsmw_hlsl.cpp" "$R/XenosRecomp/shader_recompiler.cpp" -o "$B/xenos_hlsl" || exit 1

rm -rf "$S"; mkdir -p "$S/spirv"

SUELTOS=${SUELTOS:-$RAIZ/app/out/shaders_fh2_sueltos}

# Manifesto para comparar builds entre PCs (versões + contagens)
{
  echo "dxc=$DXC"
  "$DXC" --version 2>/dev/null | head -1
  echo "spirv_val=$VAL"
  "$VAL" --version 2>/dev/null | head -1
  echo "spirv_opt=$OPT"
  "$OPT" --version 2>/dev/null | head -1
  echo "imagens=$(ls "$RAIZ"/app/out/shaders_fh2_imagem.* 2>/dev/null | wc -l)"
  echo "sueltos=$(ls "$SUELTOS"/*.bin 2>/dev/null | wc -l)"
} > "$S/versoes.txt" 2>/dev/null || true

python3 "$RAIZ/tools/xenos/extrair_fxobj_pistas.py" "$RAIZ/assets/game_root" "$S/fxobj_pistas" "$B/lzx_xmem"

# Containers do executável + pistas
python3 "$RAIZ/tools/xenos/extrair_contenedores.py" "$RAIZ/assets/game_root" "$S/contenedores" "$S/fxobj_pistas" \
  "${IMAGENS[@]}"

# Shaders soltos da D3D library (dump do renderizador nativo). Sem eles a biblioteca
# sai menor e sem os shaders internos da D3D (diferenças visuais): gere com o jogo
# rodando no modo nativo antes de rebuildar:
#   tools/rodar_pc.sh --fh2_nativo_volcar_sueltos=app/out/shaders_fh2_sueltos
if [ -d "$SUELTOS" ]; then
  python3 "$RAIZ/tools/xenos/d3d_internos.py" "$SUELTOS" "$S/contenedores"
else
  echo "[aviso] sem $SUELTOS: a biblioteca sai sem os shaders internos da D3D." >&2
  echo "        Gere com: tools/rodar_pc.sh --fh2_nativo_volcar_sueltos=$SUELTOS" >&2
fi

# xenos_hlsl retorna 2 quando algum shader é saltado (normal); só falha de verdade com outro código
# rc=0 + || : com set -e, `cmd; rc=$?` abortaria se cmd falhasse (o ; não protege)
rc=0
"$B/xenos_hlsl" "$S/contenedores" "$S/hlsl" "$COMUN" > "$S/traduccion.log" 2>&1 || rc=$?
tail -1 "$S/traduccion.log"
if [ $rc -ne 0 ] && [ $rc -ne 2 ]; then exit $rc; fi
# grep sem match retorna 1: com pipefail isso abortaria o script (é só informativo)
grep "rechazada\|no produjo\|no pude\|firma" "$S/traduccion.log" | sed -E 's/^ *[pv]_[0-9]+(\.bin)?:? *//' |
  sort | uniq -c | sort -rn | head -5 || true

: > "$S/dxc.log"; ok=0; fail=0
for f in "$S"/hlsl/*.hlsl; do
  b=$(basename "$f" .hlsl)
  if [[ $b == p_* ]]; then t=ps_6_6; e=(); else t=vs_6_6; e=(-fvk-invert-y); fi
  if "$DXC" -spirv -T "$t" -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout "${e[@]}" \
     -Fo "$S/spirv/$b.spv" "$f" >> "$S/dxc.log" 2>&1 &&
     "$OPT" --strip-debug --strip-nonsemantic "$S/spirv/$b.spv" -o "$S/spirv/$b.spv" >> "$S/dxc.log" 2>&1 &&
     "$VAL" --target-env vulkan1.2 --scalar-block-layout "$S/spirv/$b.spv" >> "$S/dxc.log" 2>&1; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1)); rm -f "$S/spirv/$b.spv"; echo "FALHOU $b" >> "$S/dxc.log"
  fi
done
echo "spirv ok=$ok falhou=$fail"
grep -h "error:" "$S/dxc.log" | sed -E 's/.*error: //' | sort | uniq -c | sort -rn | head -6 || true

"$CXX" -std=c++23 -O1 -DFH2_RECOMP -I"$RAIZ/app/src/nativo" -I"$TP/xxHash" -I"$TP/fmt/include" -DFMT_HEADER_ONLY \
  "$RAIZ/tools/xenos/empaquetar.cpp" "$RAIZ/app/src/nativo/nfsmw_shader_library.cpp" -o "$B/empaquetar" || exit 1
"$B/empaquetar" "$S/contenedores" "$S/spirv" "$S/fh2_shaders.nfsp" && cp "$S/fh2_shaders.nfsp" "$RAIZ/app/out/linux/"
{
  echo "contenedores=$(ls "$S/contenedores"/*.bin 2>/dev/null | wc -l)"
  echo "hlsl=$(ls "$S/hlsl"/*.hlsl 2>/dev/null | wc -l)"
  echo "spirv=$(ls "$S/spirv"/*.spv 2>/dev/null | wc -l)"
  echo "spirv_ok=$ok spirv_falhou=$fail"
  echo "nfsp_bytes=$(stat -c%s "$S/fh2_shaders.nfsp" 2>/dev/null)"
} >> "$S/versoes.txt" 2>/dev/null || true
cat "$S/versoes.txt"
# cobertura é só informativa (o .xsh só existe em PC que já rodou o jogo): não aborta o build
python3 "$RAIZ/tools/xenos/cobertura.py" "$XSH" "$S/contenedores" "$S/spirv" ||
  echo "[aviso] cobertura indisponível (sem $XSH)"
