#!/bin/bash
# Ferramentas EXATAS do pipeline de shaders (DXC + SPIR-V do NDK), com download ou
# instalação automática quando ausentes ou de versão diferente. Para "source", não execute:
#
#   source "$RAIZ/tools/xenos/ferramentas.sh"
#   ferramentas_resolver   # define DXC, VAL, OPT e imprime [versao]
#
# Nada daqui vai para o Git: os binários são baixados para fora do repositório
# (~/.local/share/fh2-tools, ~/.cache/fh2-tools e o Android SDK). Cada jogador gera a
# própria biblioteca com o MESMO toolchain em qualquer PC, sem copiar nada manualmente.
#
# Requer: curl ou wget, sha256sum, tar, java (para o sdkmanager) e o Android SDK com
# cmdline-tools (para instalar o NDK exato).

# ---- Versões travadas (só mude testando a biblioteca inteira nos PCs de referência) ----
FH2_DXC_TAG="v1.9.2607"      # release em github.com/microsoft/DirectXShaderCompiler
FH2_DXC_MARCA="0d3ee6b5"     # commit que aparece na saída de `dxc --version`
FH2_DXC_ARQUIVO="linux_dxc_2026_07_29.x86_x64.tar.gz"
FH2_DXC_URL="https://github.com/microsoft/DirectXShaderCompiler/releases/download/${FH2_DXC_TAG}/${FH2_DXC_ARQUIVO}"
FH2_DXC_SHA256="55665c87824051ed4774ff3280a79ccbbb7d39243b9736ca5e98222134112d54"
FH2_DXC_DIR="$HOME/.local/share/fh2-tools/dxc-1.9.2607"

FH2_NDK_VERSAO="29.0.14206865"   # pacote do sdkmanager: "ndk;<versão>"
FH2_SPIRV_MARCA="ndk-r28b"       # aparece na saída de `spirv-val --version`

ferramentas_baixar() { # $1=url $2=destino
  if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 -o "$2" "$1"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "$2" "$1"
  else
    echo "ERRO: nem curl nem wget encontrados; instale um deles." >&2
    return 1
  fi
}

ferramentas_dxc_ok() { # $1=caminho do dxc
  [ -n "${1:-}" ] && [ -x "$1" ] && "$1" --version 2>/dev/null | grep -q "$FH2_DXC_MARCA"
}

ferramentas_resolver_dxc() {
  # 1. DXC explícito no ambiente, somente se for a versão travada
  if ferramentas_dxc_ok "${DXC:-}"; then return 0; fi
  if [ -n "${DXC:-}" ]; then
    echo "[aviso] DXC=$DXC não é a versão travada ($FH2_DXC_TAG); será baixada a exata." >&2
  fi
  # 2. Instalação travada já baixada antes (ls sem match + pipefail abortaria: || true)
  local achado
  achado=$(ls -d "$FH2_DXC_DIR"/*/bin/dxc 2>/dev/null | head -1 || true)
  if ferramentas_dxc_ok "$achado"; then DXC="$achado"; return 0; fi
  # 3. Baixar a versão exata do GitHub (download único de ~490 MB)
  echo "[info] baixando DXC $FH2_DXC_TAG (~490 MB)..."
  mkdir -p "$FH2_DXC_DIR" "$HOME/.cache/fh2-tools"
  local tgz="$HOME/.cache/fh2-tools/$FH2_DXC_ARQUIVO"
  if [ ! -f "$tgz" ]; then
    ferramentas_baixar "$FH2_DXC_URL" "$tgz" || return 1
  fi
  echo "$FH2_DXC_SHA256  $tgz" | sha256sum -c - || {
    echo "ERRO: sha256 do DXC não confere; apague $tgz e tente de novo." >&2
    return 1
  }
  local tmp="$HOME/.cache/fh2-tools/dxc-extrair"
  rm -rf "$tmp"; mkdir -p "$tmp"
  tar -xzf "$tgz" -C "$tmp" || return 1
  achado=$(find "$tmp" -maxdepth 3 -path "*/bin/dxc" 2>/dev/null | head -1 || true)
  if ! ferramentas_dxc_ok "$achado"; then
    echo "ERRO: DXC extraído mas a versão não confere." >&2
    return 1
  fi
  local raiz_pkg nome_pkg
  raiz_pkg=$(dirname "$(dirname "$achado")")
  nome_pkg=$(basename "$raiz_pkg")
  rm -rf "$FH2_DXC_DIR/$nome_pkg"
  mv "$raiz_pkg" "$FH2_DXC_DIR/$nome_pkg"
  rm -rf "$tmp"
  DXC="$FH2_DXC_DIR/$nome_pkg/bin/dxc"
  return 0
}

ferramentas_spirv_ok() { # $1=spirv-val $2=spirv-opt
  [ -n "${1:-}" ] && [ -x "$1" ] && [ -n "${2:-}" ] && [ -x "$2" ] &&
    "$1" --version 2>/dev/null | grep -q "$FH2_SPIRV_MARCA"
}

ferramentas_sdk_dir() {
  if [ -n "${ANDROID_HOME:-}" ] && [ -d "$ANDROID_HOME" ]; then echo "$ANDROID_HOME"; return 0; fi
  if [ -n "${ANDROID_SDK_ROOT:-}" ] && [ -d "$ANDROID_SDK_ROOT" ]; then echo "$ANDROID_SDK_ROOT"; return 0; fi
  if [ -d "$HOME/Android/Sdk" ]; then echo "$HOME/Android/Sdk"; return 0; fi
  return 1
}

ferramentas_resolver_spirv() {
  # 1. Overrides explícitos, somente se forem a versão travada
  if ferramentas_spirv_ok "${SPIRV_VAL:-}" "${SPIRV_OPT:-}"; then
    VAL="${SPIRV_VAL}"; OPT="${SPIRV_OPT}"; return 0
  fi
  if [ -n "${SPIRV_VAL:-}${SPIRV_OPT:-}" ]; then
    echo "[aviso] SPIRV_VAL/SPIRV_OPT não são a versão travada (NDK $FH2_NDK_VERSAO); ignorando." >&2
  fi
  local sdk ndkdir val opt sdkm c
  sdk=$(ferramentas_sdk_dir) || {
    echo "ERRO: Android SDK não encontrado (ANDROID_HOME, ANDROID_SDK_ROOT ou ~/Android/Sdk)." >&2
    echo "      Instale o Android SDK com cmdline-tools e rode de novo: o NDK exato será instalado sozinho." >&2
    return 1
  }
  ndkdir="$sdk/ndk/$FH2_NDK_VERSAO"
  val="$ndkdir/shader-tools/linux-x86_64/spirv-val"
  opt="$ndkdir/shader-tools/linux-x86_64/spirv-opt"
  if ferramentas_spirv_ok "$val" "$opt"; then VAL="$val"; OPT="$opt"; return 0; fi
  # 2. Instalar o NDK exato via sdkmanager (outras versões/shims são ignorados)
  sdkm=""
  for c in "$sdk/cmdline-tools/latest/bin/sdkmanager" "$sdk/cmdline-tools/bin/sdkmanager" "$sdk/tools/bin/sdkmanager"; do
    if [ -x "$c" ]; then sdkm="$c"; break; fi
  done
  # condição de if é isenta do set -e: seguro mesmo quando sdkm já está setado
  if [ -z "$sdkm" ] && command -v sdkmanager >/dev/null 2>&1; then sdkm="sdkmanager"; fi
  if [ -z "$sdkm" ]; then
    echo "ERRO: NDK $FH2_NDK_VERSAO ausente e sdkmanager não encontrado em $sdk." >&2
    echo "      Instale o Android cmdline-tools e rode de novo." >&2
    return 1
  fi
  echo "[info] instalando Android NDK $FH2_NDK_VERSAO via sdkmanager (pode demorar)..."
  yes | "$sdkm" --licenses > /dev/null 2>&1 || true
  "$sdkm" "ndk;$FH2_NDK_VERSAO" || return 1
  if ferramentas_spirv_ok "$val" "$opt"; then VAL="$val"; OPT="$opt"; return 0; fi
  echo "ERRO: NDK instalado mas spirv-val/opt não conferem a versão travada." >&2
  return 1
}

# Ponto de entrada: resolve tudo e imprime as versões usadas.
ferramentas_resolver() {
  ferramentas_resolver_dxc || return 1
  ferramentas_resolver_spirv || return 1
  reportar_versao "DXC" "$DXC"
  reportar_versao "spirv-val" "$VAL"
  reportar_versao "spirv-opt" "$OPT"
}
