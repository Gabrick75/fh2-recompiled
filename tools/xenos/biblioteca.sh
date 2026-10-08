#!/bin/bash
# Builds FH2's translated shader library for the native renderer ("biblioteca" means "library") and measures how many
# of the shaders the game really loaded (the emulator's shader storage, .xsh) it covers:
#   1. unpacks the track .fxobj from the zips and extracts every shader container;
#   2. translates them to HLSL with XenosRecomp as changed by nfsmw-nx and this project (tools/xenos/recomp);
#   3. compiles to SPIR-V with DXC and validates with spirv-val;
#   4. coverage (cobertura.py).
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
XSH=${XSH:-$RAIZ/app/out/run/user/cache/shaders/shareable/4D530AA4.xsh}
COMUN=${COMUN:-$R/XenosRecomp/shader_common.h}
TP=$RAIZ/sdk/thirdparty; M=$TP/libmspack/libmspack/mspack
mkdir -p $B
[ -x $B/lzx_xmem ] || gcc -O1 -I$M $RAIZ/tools/xenos/lzx_xmem.c $M/lzxd.c $M/system.c -o $B/lzx_xmem || exit 1
g++ -std=c++23 -O1 -I$R -I$R/XenosRecomp -I$TP/fmt/include -I$TP/xxHash -include $R/pch_min.h -DFMT_HEADER_ONLY \
  -DXXH_INLINE_ALL -DNFSMW_RECOMP -DFH2_RECOMP $R/nfsmw_hlsl.cpp $R/XenosRecomp/shader_recompiler.cpp -o $B/xenos_hlsl || exit 1
rm -rf $S; mkdir -p $S/spirv
python3 $RAIZ/tools/xenos/extrair_fxobj_pistas.py $RAIZ/assets/game_root $S/fxobj_pistas $B/lzx_xmem
python3 $RAIZ/tools/xenos/extrair_contenedores.py $RAIZ/assets/game_root $S/contenedores $S/fxobj_pistas
$B/xenos_hlsl $S/contenedores $S/hlsl $COMUN > $S/traduccion.log 2>&1; tail -1 $S/traduccion.log
grep "rechazada\|no produjo\|no pude\|firma" $S/traduccion.log | sed -E 's/^ *[pv]_[0-9]+(\.bin)?:? *//' |
  sort | uniq -c | sort -rn | head -5
: > $S/dxc.log; ok=0; fail=0
for f in $S/hlsl/*.hlsl; do
  b=$(basename $f .hlsl)
  if [[ $b == p_* ]]; then t=ps_6_6; e=(); else t=vs_6_6; e=(-fvk-invert-y); fi
  if "$DXC" -spirv -T $t -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout "${e[@]}" \
       -Fo $S/spirv/$b.spv $f >> $S/dxc.log 2>&1 &&
     "$VAL" --target-env vulkan1.2 --scalar-block-layout $S/spirv/$b.spv >> $S/dxc.log 2>&1; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1)); rm -f $S/spirv/$b.spv; echo "FALHOU $b" >> $S/dxc.log
  fi
done
echo "spirv ok=$ok falhou=$fail"
grep -h "error:" $S/dxc.log | sed -E 's/.*error: //' | sort | uniq -c | sort -rn | head -6
python3 $RAIZ/tools/xenos/cobertura.py "$XSH" $S/contenedores $S/spirv
