# Tutorial — Build do Android e biblioteca de shaders (português)

Passo a passo, do clone do repositório até o jogo rodando no modo nativo no Motorola Moto G9 Play (Adreno 610)
com o fallback sem descriptor indexing (método A), sem precisar do driver Turnip.

## 1. Pré-requisitos (PC Linux)

```sh
sudo apt update
sudo apt install -y git clang-20 clang++-20 cmake ninja-build python3 openjdk-17-jdk unzip wget curl
```

Verifique:

```sh
clang-20 --version   # >= 20
cmake --version      # >= 3.25 (PC) / 3.30.5 (Android)
ninja --version
java -version        # 17+
```

## 2. Clonar o repositório

```sh
git clone https://github.com/Gabrick75/fh2-recompiled.git
cd fh2-recompiled
```

Entre na branch com o método A (fallback sem descriptor indexing):

```sh
git fetch origin
git checkout -b compat/nativo-sem-indexing origin/compat/nativo-sem-indexing 2>/dev/null ||
git checkout -b compat/nativo-sem-indexing
```

Se não houver remote/branch, aplique os commits do método A manualmente no seu histórico local. O código do fallback está em:
- `app/src/nativo/nfsmw_nativo_dibujos.cpp`
- `app/src/nativo/nfsmw_spirv_descriptores.h`
- `android/app/src/main/cpp/android_diagnostico.cpp`

## 3. Baixar fontes de terceiros (SDK ReXGlue)

```sh
python3 tools/fetch_thirdparty.py
```

Baixa o ReXGlue SDK e dependências em `sdk/thirdparty`. Demora alguns minutos.

## 4. Extrair sua própria cópia do jogo

Você precisa do seu ISO legal do Forza Horizon 2 (Xbox 360, USA, Title ID `4D530AA4`). O repositório não contém arquivos do jogo.

```sh
python3 tools/fase1_extraer.py /caminho/para/SeuForzaHorizon2.iso -o assets/game_root
```

A pasta `assets/game_root/` é ignorada pelo Git e nunca deve ser compartilhada.

## 5. Rodar o codegen no PC (gera `app/generated`)

```sh
cmake -S app -B app/out/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
      -DREXSDK_DIR="$PWD/sdk"
cmake --build app/out/linux
```

Isso gera `app/generated/default/sources.cmake`, obrigatório para o build do Android.

## 6. Instalar ferramentas para gerar a biblioteca de shaders

A biblioteca `fh2_shaders.nfsp` precisa de DXC e spirv-val/opt.

**DXC** (HLSL → SPIR-V):

```sh
mkdir -p ~/.local/share/fh2-tools
# Baixe um release do DXC (https://github.com/microsoft/DirectXShaderCompiler)
# e coloque em: ~/.local/share/fh2-tools/dxc-<versao>/bin/dxc
```

**spirv-val / spirv-opt** (já vêm no Android NDK):

```sh
ls $HOME/Android/Sdk/ndk/28.2.13676358/shader-tools/linux-x86_64/
# deve conter spirv-val e spirv-opt
```

## 7. Gerar a biblioteca de shaders (modo nativo)

```sh
export DXC=~/.local/share/fh2-tools/dxc-*/bin/dxc   # ajuste o caminho real
tools/xenos/biblioteca.sh
```

O que acontece:
1. Descompacta `.fxobj` das pistas (LZX).
2. Extrai contêineres de shader do executável e das pistas.
3. Traduz para HLSL com o XenosRecomp (`FH2_RECOMP`).
4. Compila com DXC para SPIR-V Vulkan 1.2.
5. Empacota em `app/out/shaders_fh2/fh2_shaders.nfsp`.

Saída esperada: algo como `spirv ok=2559 falhou=0` e o arquivo `fh2_shaders.nfsp` (~72 MB).

> **Importante:** esta biblioteca é feita a partir dos seus arquivos do jogo. Não compartilhe nem commite.

## 8. Build do Android APK

```sh
export ANDROID_HOME="$HOME/Android/Sdk"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
./build_android.sh
```

O APK fica em:

```
android/app/build/outputs/apk/release/app-release.apk
```

## 9. Instalar somente no Moto G9 Play via ADB

```sh
adb devices -l
# confirme que aparece 0074545453 (moto_g_9__play)

adb -s 0074545453 install -r android/app/build/outputs/apk/release/app-release.apk
```

## 10. Preparar os arquivos do jogo no celular

Copie a pasta extraída ou o ISO para o armazenamento do celular:

```sh
# opção A: pasta extraída
adb -s 0074545453 push assets/game_root /sdcard/jogos/ForzaHorizon2

# opção B: ISO (se preferir ler direto da imagem)
adb -s 0074545453 push /caminho/para/SeuForzaHorizon2.iso /sdcard/jogos/
```

E a biblioteca de shaders:

```sh
adb -s 0074545453 push app/out/shaders_fh2/fh2_shaders.nfsp /sdcard/jogos/ForzaHorizon2/
```

A biblioteca deve ficar ao lado dos arquivos do jogo (ou ao lado do ISO), pois o renderizador nativo a carrega de `fh2_nativo_biblioteca_dir`.

## 11. Configurar o launcher no celular

Abra o app no Moto G9 Play e defina:

| Ajuste | Valor |
|--------|-------|
| Renderizador | **Native** |
| Resolução | 720p ou menor (50% recomendado no Adreno 610) |
| Anti-aliasing | Off |
| Driver de GPU | Sistema (vazio) — não precisa de Turnip para o fallback |
| FPS | 30 |

No arquivo de opções (ou via menu), garanta que está ativo:

```
--nfsmw_renderizador=nativo
--fh2_nativo_sin_indexing=true     # FORÇA o fallback sem descriptor indexing
```

O `sin_indexing` auto-liga quando falta `runtimeDescriptorArray` (caso do driver stock do Adreno 610), mas forçar com `true` garante o caminho A mesmo que a detecção mude.

## 12. Iniciar o jogo

Toque em **Jogar**. O renderizador nativo deve agora inicializar no Adreno 610 sem exigir o Turnip.

Se a tela continuar preta, gere o diagnóstico e confira o log:

```sh
# no launcher, About > Diagnostics > criar relatório
adb -s 0074545453 pull /sdcard/jogos/fh2-logs/fh2_$(date +%Y%m%d)_*.log .
```

Procure por:
- `[nativo] C6: sin descriptor indexing: metodo A (heaps com tamano fijo)` — fallback ativo.
- `spirv ok=` — biblioteca carregada.
- Ausência de `el dispositivo Vulkan no tiene runtimeDescriptorArray: no se dibuja`.

## Comandos resumidos

```sh
# 1. clone + branch
git clone https://github.com/Gabrick75/fh2-recompiled.git
cd fh2-recompiled
git checkout -b compat/nativo-sem-indexing

# 2. thirdparty
python3 tools/fetch_thirdparty.py

# 3. extrair jogo (seu ISO)
python3 tools/fase1_extraer.py SeuFH2.iso -o assets/game_root

# 4. codegen PC
cmake -S app -B app/out/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DREXSDK_DIR="$PWD/sdk"
cmake --build app/out/linux

# 5. biblioteca shaders
export DXC=~/.local/share/fh2-tools/dxc-*/bin/dxc
tools/xenos/biblioteca.sh

# 6. build APK
export ANDROID_HOME="$HOME/Android/Sdk"
./build_android.sh

# 7. instalar só no Moto G9 Play
adb -s 0074545453 install -r android/app/build/outputs/apk/release/app-release.apk

# 8. enviar dados e shader
adb -s 0074545453 push assets/game_root /sdcard/jogos/ForzaHorizon2
adb -s 0074545453 push app/out/shaders_fh2/fh2_shaders.nfsp /sdcard/jogos/ForzaHorizon2/
```

A partir daí, configure **Native** no launcher e jogue.
