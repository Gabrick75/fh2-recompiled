# Comparação: o nosso renderizador nativo × o FH2 no SDK do Pinyon Shift (ShiftGlue)

Branch `comparacao/shiftglue`, 2026-10-13. Protótipo em `prototipos/fh2-shiftglue`.

## O que foi comparado

- **[Pinyon Shift](https://github.com/arcanite24/pinyon-shift)** recompila o Forza Horizon 1 de Xbox 360 (mesma engine
  do FH2) com o **[ShiftGlue](https://github.com/arcanite24/shiftglue-sdk)**, fork BSD-3 do ReXGlue 0.10.0, a mesma
  versão do nosso SDK. O "renderizador nativo" deles é o `Fh1NativeExecutor`
  (`src/graphics/vulkan/fh1_native_executor.cpp`): o backend Vulkan do Xenia com a EDRAM refeita (superfícies por
  base/pitch/formato/MSAA, mapa de posse das 2048 baldosas com transferências entre aliases, resolves direto nas
  texturas, clears dobrados). Os shaders são traduzidos em tempo de execução; não há biblioteca pré-compilada nem
  nenhum endereço do FH1 no executor.
- **Protótipo**: o FH2 compilado com o SDK deles, sem mudar o código do jogo. Usa o nosso manifesto
  (`fh2_manifest.toml`), os nossos overrides, as correções do FH2 (`src/fh2_correcoes.cpp`) e o nosso launcher
  Android, com o pacote `com.forzahorizon2.shiftglue` ("FH2 ShiftGlue (teste)") para instalar ao lado do app.

## Resultados

### Imagem
- **PC (RADV Renoir)**: o FH2 renderizou certo de primeira na rota de teste noturna (noite escura, faróis no chão,
  HUD, folhagem), com e sem `fh1_untile_predicated_tiling`; `skips={}` em todos os relatórios do executor.
- **S23 Ultra (Adreno 740, driver da Qualcomm)**: segundo o usuário, "graficamente perfeito".

### Desempenho no S23 Ultra (mesmo trajeto, 1x, sem MSAA, limite de 30 fps)

| | Nosso nativo (`fh2_20261013_173715`, ~3 min) | ShiftGlue (`173106` e `173441`, ~1,5 min cada) |
|---|---|---|
| FPS dirigindo | mediana ~26 (17–30) | 12,8 e 13,2 (contagem do executor a cada 600 quadros) |
| GPU ocupada | ~70–80 % | ~85–93 % |
| Thread mais ocupada | 56–60 % (anel de comandos do nativo) | ~90 % (thread do jogo ou envio à GPU) |
| RAM do jogo | 0,8–1,6 GB | 0,7–1,5 GB |
| Renderizações por quadro | — | ~470–525 (o FH1 deles, otimizado: ~210) |

- O nosso fica quase sempre no teto de 30 e ainda tem folga; as quedas a 17–20 fps são com a GPU em ~75 % (o S23
  Ultra tem a GPU limitada pela Samsung).
- O ShiftGlue está preso na GPU: >85 % para 13 fps. Para 30 fps precisaria cortar mais da metade do trabalho. O
  próprio projeto só chegou a 60 fps no FH1 com sombras desligadas, reflexos a 1/4 e o driver Turnip.

### Jogabilidade (teste do usuário no mesmo trecho)
- **ShiftGlue: nenhum carro ou parede atravessado**, "como se realmente fosse no Xbox 360, sem nenhum glitch".
- **Nosso app (nativo e emulado): o carro atravessa carros e paredes** (problema conhecido desde a 0.1.0).
- Como os dois rodam o mesmo jogo com o mesmo manifesto e as mesmas correções, **a falha de colisão vem do nosso SDK
  ou do nosso gerador de código** (o que o ShiftGlue mudou ou corrigiu em relação ao ReXGlue que temos), não do
  renderizador. Os dois SDKs podem ser comparados para achar a diferença.

## Decisão

Continuar no **nosso renderizador nativo**: tem o dobro do FPS do executor deles no S23 Ultra, com folga. O ShiftGlue
fica como **referência**: imagem certa para comparar erros do nosso nativo e física/colisão certas para achar o que
falta no nosso SDK.

## Como reproduzir o protótipo

Pastas fora deste repositório (caminhos absolutos no protótipo): `referencias/shiftglue-sdk` (clone com submódulos,
inclusive o `linkernsbypass` do libadrenotools) e `referencias/fh2-shiftglue` (esta pasta `prototipos/fh2-shiftglue`
mais o código gerado).

1. SDK no PC: `cmake --preset linux-amd64 -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20`,
   `cmake --build out/build/linux-amd64 --config Release`.
2. Codegen com o `rexglue` deles: `rexglue codegen fh2_manifest.toml` (8 min, 719 arquivos), depois
   `python3 tabela_registro.py generated/default/fh2_register.cpp`.
3. PC: `cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-march=x86-64-v2
   -DCMAKE_CXX_FLAGS=-march=x86-64-v2 -DREXSDK_DIR=<shiftglue-sdk>`; rodar com `--gpu_plugin=fh1`.
4. Android: `build_apk.sh` (NDK 29.0.14206865, minSdk 33).

O que foi preciso mudar para o Android (está no protótipo):
- `GPU_PLUGINS fh1` e `--gpu_plugin=fh1`; limite de FPS por `pinyon_shift_fh1_render_fps_limit`.
- O SDL é estático no `rexruntime`: `GameActivity.getLibraries()` = `c++_shared`, `rexruntime`, `main`.
- O runtime procura `librexgpu-fh1.so` como arquivo: `packaging.jniLibs.useLegacyPackaging true`.
- NDK 29 e minSdk 33 (`std::from_chars` de ponto flutuante).
- Sem codegen no build do Android (`rexglue_proto.cmake`): o código é gerado no PC.
- `fh2_register.cpp` como tabela (`tabela_registro.py`): a função única travava o compilador do NDK.
- Controles de toque por um driver de entrada próprio (`src/fh2_toque.cpp`); medidor de FPS (`src/fh2_medidor.cpp`),
  mas o contador do SDK deles (`kSourceFrameCount`) fica em 0 no Android, então o FPS saiu dos relatórios do executor.
- Sem o teste de driver de GPU (o carregador de drivers deles é outro).
