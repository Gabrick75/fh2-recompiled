#!/bin/bash
# Recompiles one translated shader of the library (app/out/shaders_fh2/hlsl/<name>.hlsl, after editing it by hand
# for a test) and repacks fh2_shaders.nfsp, without translating everything again ("recompilar um" = recompile one).
#   tools/xenos/recompilar_um.sh p_003929 [v_003899 ...]
set -eu
RAIZ=$(cd "$(dirname "$0")/../.." && pwd)
S=$RAIZ/app/out/shaders_fh2; B=$RAIZ/app/out/xenos_bin
DXC=${DXC:-$(ls -d $HOME/.local/share/fh2-tools/dxc-*/*/bin/dxc | tail -1)}
VAL=${SPIRV_VAL:-$(ls $HOME/Android/Sdk/ndk/*/shader-tools/linux-x86_64/spirv-val | tail -1)}
OPT=${SPIRV_OPT:-$(dirname "$VAL")/spirv-opt}
for b in "$@"; do
  if [[ $b == p_* ]]; then t=ps_6_6; e=(); else t=vs_6_6; e=(-fvk-invert-y); fi
  "$DXC" -spirv -T $t -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout "${e[@]}" \
    -Fo $S/spirv/$b.spv $S/hlsl/$b.hlsl
  "$OPT" --strip-debug --strip-nonsemantic $S/spirv/$b.spv -o $S/spirv/$b.spv
  "$VAL" --target-env vulkan1.2 --scalar-block-layout $S/spirv/$b.spv
done
rm -f $S/fh2_shaders.nfsp; $B/empaquetar $S/contenedores $S/spirv $S/fh2_shaders.nfsp && cp $S/fh2_shaders.nfsp $RAIZ/app/out/linux/
