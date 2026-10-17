# Tutorial — Build do Android e biblioteca de shaders (português)

Passo a passo, do clone do repositório até o jogo rodando no modo nativo em um dispositivo Android
com o fallback sem descriptor indexing (método A), sem precisar do driver Turnip.

## 1. Pré-requisitos (PC Linux)

```sh
sudo apt update
sudo apt install -y git clang-20 clang++-20 cmake ninja-build python3 openjdk-17-jdk unzip wget curl \
  libasound2-dev libdbus-1-dev libdecor-0-dev libegl1-mesa-dev libpipewire-0.3-dev libpulse-dev \
  libudev-dev libwayland-dev libx11-dev libx11-xcb-dev libxcursor-dev libxext-dev libxfixes-dev libxi-dev \
  libxkbcommon-dev libxrandr-dev libxss-dev libxtst-dev
```

(Os pacotes `lib*` são as dependências de build do SDL3/X11; sem eles o configure do SDK falha. O `./build_pc.sh` instala sozinho o que faltar.)

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

O código do fallback sem descriptor indexing (método A) já está na branch `main`:
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

## 5. Build do PC: SDK host, codegen e app (gera `app/generated`)

Um comando só faz tudo (compila o `rexglue` host do SDK se preciso, roda o codegen sobre o seu jogo e compila o app; demora na primeira vez):

```sh
./build_pc.sh
```

Isso gera `app/generated/rexglue.cmake` e `app/generated/default/sources.cmake`, obrigatórios para o build do Android.

(O script usa `-march=x86-64-v3`, igual ao PC de referência — o SDK exige SSSE3+. Se a sua CPU for anterior a ~2013, rode `MARCH=x86-64-v2 ./build_pc.sh`.)

Depois, gere o despejo da imagem do executável (**obrigatório** para o passo 7; abre o jogo no PC — espere iniciar e feche):

```sh
tools/rodar_pc.sh --xex_despejar_imagem=app/out/shaders_fh2_imagem
```

## 6. Ferramentas da biblioteca de shaders (automático)

A biblioteca `fh2_shaders.nfsp` precisa de DXC (HLSL → SPIR-V) e spirv-val/opt. **Nada manual:** o `tools/xenos/biblioteca.sh` baixa sozinho as versões exatas e travadas (iguais em todo PC):
- DXC `v1.9.2607` do GitHub (~490 MB, download único, com checagem sha256);
- SPIR-V do NDK `29.0.14206865`, instalado sozinho via `sdkmanager` (precisa do Android SDK com cmdline-tools e Java).

Se as versões locais divergirem, o script avisa, baixa/instala as exatas e segue. Não instale DXC nem NDK manualmente para isso.

## 7. Gerar a biblioteca de shaders (modo nativo)

```sh
tools/xenos/biblioteca.sh   # baixa sozinho o toolchain exato (DXC + NDK)
```

O que acontece:
1. Descompacta `.fxobj` das pistas (LZX).
2. Extrai contêineres de shader do executável e das pistas.
3. Traduz para HLSL com o XenosRecomp (`FH2_RECOMP`).
4. Compila com DXC para SPIR-V Vulkan 1.2.
5. Empacota em `app/out/shaders_fh2/fh2_shaders.nfsp`.

Saída esperada: `spirv ok=4074 falhou=4` e o arquivo `fh2_shaders.nfsp` (~72 MB).

> **Importante:** esta biblioteca é feita a partir dos seus arquivos do jogo. Não compartilhe nem commite.

### Shaders internos da D3D (sueltos): biblioteca completa em duas passadas

A primeira biblioteca sai sem os shaders internos da D3D (o script avisa `[aviso] sem .../shaders_fh2_sueltos`). Ela já abre o jogo — use-a para gerar o despejo que falta, com o jogo em modo nativo (navegue nos menus/jogue um pouco para o renderizador encontrar os shaders; depois feche):

```sh
tools/rodar_pc.sh --nfsmw_renderizador=nativo --fh2_nativo_volcar_sueltos=$PWD/app/out/shaders_fh2_sueltos
```

Depois rebuild para a biblioteca completa:

```sh
tools/xenos/biblioteca.sh
```

Conferência (valores de referência em `app/out/shaders_fh2/versoes.txt`): `contenedores=4142`, `hlsl=4078`, `spirv ok=4074 falhou=4`, `sueltos=42`, `nfsp_bytes=75497272`.

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

## 9. Instalar no dispositivo Android via ADB

```sh
adb devices -l
# confirme o serial do seu dispositivo na lista

adb -s <SEU_SERIAL> install -r android/app/build/outputs/apk/release/app-release.apk
```

## 10. Preparar os arquivos do jogo no dispositivo

Copie a pasta extraída ou o ISO para o armazenamento do dispositivo:

```sh
# opção A: pasta extraída
adb -s <SEU_SERIAL> push assets/game_root /sdcard/jogos/ForzaHorizon2

# opção B: ISO (se preferir ler direto da imagem)
adb -s <SEU_SERIAL> push /caminho/para/SeuForzaHorizon2.iso /sdcard/jogos/
```

E a biblioteca de shaders:

```sh
adb -s <SEU_SERIAL> push app/out/shaders_fh2/fh2_shaders.nfsp /sdcard/jogos/ForzaHorizon2/
```

A biblioteca deve ficar ao lado dos arquivos do jogo (ou ao lado do ISO), pois o renderizador nativo a carrega de `fh2_nativo_biblioteca_dir`.

## 11. Configurar o launcher no dispositivo

Abra o app no dispositivo Android e defina:

| Ajuste | Valor |
|--------|-------|
| Renderizador | **Native** |
| Resolução | 720p ou menor (recomendado 50% ou menos em GPUs Adreno) |
| Anti-aliasing | Off |
| Driver de GPU | Sistema (vazio) — não precisa de Turnip para o fallback |
| FPS | 30 |

No arquivo de opções (ou via menu), garanta que está ativo:

```
--nfsmw_renderizador=nativo
--fh2_nativo_sin_indexing=true     # FORÇA o fallback sem descriptor indexing
```

O `sin_indexing` auto-liga quando falta `runtimeDescriptorArray` (caso de drivers stock de algumas GPUs Adreno), mas forçar com `true` garante o caminho A mesmo que a detecção mude.

## 12. Iniciar o jogo

Toque em **Jogar**. O renderizador nativo deve inicializar sem exigir o Turnip.

Se a tela continuar preta, gere o diagnóstico e confira o log:

```sh
# no launcher, About > Diagnostics > criar relatório
adb -s <SEU_SERIAL> pull /sdcard/jogos/fh2-logs/fh2_$(date +%Y%m%d)_*.log .
```

Procure por:
- `[nativo] C6: sin descriptor indexing: metodo A (heaps com tamano fijo)` — fallback ativo.
- `spirv ok=` — biblioteca carregada.
- Ausência de `el dispositivo Vulkan no tiene runtimeDescriptorArray: no se dibuja`.

## Comandos resumidos

```sh
# 1. clone
git clone https://github.com/Gabrick75/fh2-recompiled.git
cd fh2-recompiled

# 2. thirdparty
python3 tools/fetch_thirdparty.py

# 3. extrair jogo (seu ISO)
python3 tools/fase1_extraer.py SeuFH2.iso -o assets/game_root

# 4. build do PC (SDK host + codegen + app)
./build_pc.sh

# 4b. dump da imagem do executável (obrigatório p/ biblioteca)
tools/rodar_pc.sh --xex_despejar_imagem=app/out/shaders_fh2_imagem

# 5. biblioteca shaders (toolchain exato baixado sozinho)
tools/xenos/biblioteca.sh

# 5b. sueltos da D3D + rebuild p/ biblioteca completa
tools/rodar_pc.sh --nfsmw_renderizador=nativo --fh2_nativo_volcar_sueltos=$PWD/app/out/shaders_fh2_sueltos
tools/xenos/biblioteca.sh

# 6. build APK
export ANDROID_HOME="$HOME/Android/Sdk"
./build_android.sh

# 7. instalar no dispositivo Android
adb -s <SEU_SERIAL> install -r android/app/build/outputs/apk/release/app-release.apk

# 8. enviar dados e shader
adb -s <SEU_SERIAL> push assets/game_root /sdcard/jogos/ForzaHorizon2
adb -s <SEU_SERIAL> push app/out/shaders_fh2/fh2_shaders.nfsp /sdcard/jogos/ForzaHorizon2/
```

A partir daí, configure **Native** no launcher e jogue.
