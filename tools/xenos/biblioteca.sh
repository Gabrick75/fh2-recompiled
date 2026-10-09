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
#   tools/xenos/biblioteca.sh                       DXC from DXC=..., default ~/.local/share/fh2-tools/dxc-*/
set -u
RAIZ=$(cd "$(dirname "$0")/../.." && pwd)
REC=$RAIZ/tools/xenos/recomp
R=$REC
S=$RAIZ/app/out/shaders_fh2; B=$RAIZ/app/out/xenos_bin
DXC=${DXC:-$(ls -d $HOME/.local/share/fh2-tools/dxc-*/*/bin/dxc | tail -1)}
VAL=${SPIRV_VAL:-$(ls $HOME/Android/Sdk/ndk/*/shader-tools/linux-x86_64/spirv-val | tail -1)}
OPT=${SPIRV_OPT:-$(dirname "$VAL")/spirv-opt}
XSH=${XSH:-$RAIZ/app/out/run/user/cache/shaders/shareable/4D530AA4.xsh}
COMUN=${COMUN:-$R/XenosRecomp/shader_common.h}
TP=$RAIZ/sdk/thirdparty; M=$TP/libmspack/libmspack/mspack
mkdir -p $B
[ -x $B/lzx_xmem ] || gcc -O1 -I$M $RAIZ/tools/xenos/lzx_xmem.c $M/lzxd.c $M/system.c -o $B/lzx_xmem || exit 1
g++ -std=c++23 -O1 -I$R -I$R/XenosRecomp -I$TP/fmt/include -I$TP/xxHash -include $R/pch_min.h -DFMT_HEADER_ONLY \
  -DXXH_INLINE_ALL -DNFSMW_RECOMP -DFH2_RECOMP $R/nfsmw_hlsl.cpp $R/XenosRecomp/shader_recompiler.cpp -o $B/xenos_hlsl || exit 1
rm -rf $S; mkdir -p $S/spirv
python3 $RAIZ/tools/xenos/extrair_fxobj_pistas.py $RAIZ/assets/game_root $S/fxobj_pistas $B/lzx_xmem
# The executable's modules as loaded (tools/rodar_pc.sh --xex_despejar_imagem=$RAIZ/app/out/shaders_fh2_imagem).
python3 $RAIZ/tools/xenos/extrair_contenedores.py $RAIZ/assets/game_root $S/contenedores $S/fxobj_pistas \
  $(ls $RAIZ/app/out/shaders_fh2_imagem.* 2>/dev/null)
$B/xenos_hlsl $S/contenedores $S/hlsl $COMUN > $S/traduccion.log 2>&1; tail -1 $S/traduccion.log
grep "rechazada\|no produjo\|no pude\|firma" $S/traduccion.log | sed -E 's/^ *[pv]_[0-9]+(\.bin)?:? *//' |
  sort | uniq -c | sort -rn | head -5
: > $S/dxc.log; ok=0; fail=0
for f in $S/hlsl/*.hlsl; do
  b=$(basename $f .hlsl)
  if [[ $b == p_* ]]; then t=ps_6_6; e=(); else t=vs_6_6; e=(-fvk-invert-y); fi
  if "$DXC" -spirv -T $t -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout "${e[@]}" \
       -Fo $S/spirv/$b.spv $f >> $S/dxc.log 2>&1 &&
     "$OPT" --strip-debug --strip-nonsemantic $S/spirv/$b.spv -o $S/spirv/$b.spv >> $S/dxc.log 2>&1 &&
     "$VAL" --target-env vulkan1.2 --scalar-block-layout $S/spirv/$b.spv >> $S/dxc.log 2>&1; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1)); rm -f $S/spirv/$b.spv; echo "FALHOU $b" >> $S/dxc.log
  fi
done
echo "spirv ok=$ok falhou=$fail"
grep -h "error:" $S/dxc.log | sed -E 's/.*error: //' | sort | uniq -c | sort -rn | head -6
g++ -std=c++23 -O1 -DFH2_RECOMP -I$RAIZ/app/src/nativo -I$TP/xxHash -I$TP/fmt/include -DFMT_HEADER_ONLY \
  $RAIZ/tools/xenos/empaquetar.cpp $RAIZ/app/src/nativo/nfsmw_shader_library.cpp -o $B/empaquetar || exit 1
$B/empaquetar $S/contenedores $S/spirv $S/fh2_shaders.nfsp && cp $S/fh2_shaders.nfsp $RAIZ/app/out/linux/
python3 $RAIZ/tools/xenos/cobertura.py "$XSH" $S/contenedores $S/spirv
