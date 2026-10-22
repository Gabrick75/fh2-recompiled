<div align="center">

# FH2 Recompiled

**Uma recompilação estática não oficial de Forza Horizon 2 (Xbox 360) para Android e Linux**

[English](README.md) · [Avisos de terceiros](THIRD_PARTY_NOTICES.md) · [Licença: GPL-3.0](LICENSE)

</div>

> [!IMPORTANT]
> Este é um projeto de fã, sem fins comerciais, de pesquisa em interoperabilidade e preservação. **Não** é
> afiliado, endossado nem patrocinado pela Microsoft, Xbox Game Studios, Playground Games, Turn 10 Studios ou
> qualquer outro detentor de direitos. **Este repositório não contém arquivos nem código do jogo. Os APKs dos
> Releases não contêm dados do jogo e não fazem nada sem os arquivos extraídos da sua própria cópia, obtida
> legalmente.** Veja [Aspectos legais](#aspectos-legais).

## Índice

- [Visão geral](#visão-geral)
- [Como funciona](#como-funciona)
- [Situação do projeto](#situação-do-projeto)
- [Requisitos](#requisitos)
- [Compilando](#compilando)
- [Rodando no Android](#rodando-no-android)
- [Estrutura do repositório](#estrutura-do-repositório)
- [Contribuindo](#contribuindo)
- [Créditos](#créditos)
- [Aspectos legais](#aspectos-legais)
- [Licença](#licença)

## Visão geral

O FH2 Recompiled traduz antecipadamente o código PowerPC do executável de Xbox 360 de Forza Horizon 2 para C++ e
o compila como programa nativo para celulares Android ARM64 e para Linux x86-64. Ele é construído sobre o
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), que fornece o kernel do Xbox 360, o runtime e a emulação
da GPU Xenos do console sobre Vulkan. Há também um renderizador nativo experimental, que desenha os comandos de
GPU do jogo direto em Vulkan com shaders traduzidos.

O aplicativo Android inclui um launcher com opções de gráficos e desempenho, o jogo lido direto da imagem do
disco (.iso), drivers Vulkan personalizados, uma tela de conquistas, controles de toque com editor, suporte a
controles físicos e um painel de desempenho durante o jogo.

## Como funciona

| Parte | Como roda |
|---|---|
| Código do jogo (CPU) | Recompilado antecipadamente: as instruções PowerPC do `default.xex` viram C++, compilado numa biblioteca nativa. Não há interpretador nem JIT para o código do jogo. |
| Kernel e bibliotecas do sistema do Xbox 360 | Reimplementados pelo ReXGlue SDK (derivado do projeto Xenia). |
| GPU (Xenos), emulada | O padrão. O fluxo de comandos da GPU é interpretado, a EDRAM é reproduzida com render targets Vulkan e os shaders são traduzidos para SPIR-V durante o jogo. |
| GPU, nativa (experimental) | Adaptada do [nfsmw-nx](https://github.com/StevensND/nfsmw-nx): lê o mesmo fluxo de comandos e desenha direto em Vulkan, com shaders traduzidos antes (XenosRecomp) numa biblioteca, `fh2_shaders.nfsp`, feita a partir dos seus próprios arquivos do jogo. Bem mais rápida, ainda com alguns erros de imagem. |
| Áudio, entrada, vídeo | ReXGlue SDK, SDL3 e FFmpeg. |

Ao compilar, o código gerado a partir do jogo é produzido na sua máquina, a partir da sua cópia do jogo; o
`.gitignore` o exclui deste repositório.

## Situação do projeto

Em desenvolvimento. O jogo inicia, roda os menus e as corridas, mas nos celulares ainda está longe da taxa de
quadros do console original.

O que este projeto acrescenta ao SDK:

- Um launcher Android: renderizador emulado ou nativo, resolução de renderização em porcentagem, MSAA/FXAA,
  filtro anisotrópico, qualidade das sombras (nativo), detalhe do mundo (grama, objetos, reflexos, distância das
  sombras e multidão de 0,25x a 4x do Xbox 360, todos menos objetos também desligáveis; a grama, e desligar
  sombras e multidão, no renderizador nativo), limite de FPS (de 30 a livre), modo de apresentação,
  fixação de threads, idioma do jogo e do app, um painel com FPS e uso de GPU, CPU e RAM, importar/exportar
  saves e cópias de segurança.
- Teclado e mouse como controle, reconhecidos sozinhos como um controle: o jogo os lê como um controle de Xbox
  360, com um layout de corrida, o mouse virando a câmera e toda tecla rebindável dentro do jogo.
- O jogo rodando direto da imagem do disco (.iso), sem precisar extrair.
- Drivers Vulkan personalizados (Turnip, pacotes da Qualcomm) carregados pelo libadrenotools, com um teste de
  Vulkan antes do uso.
- Uma tela de conquistas com todas as conquistas do jogo, as desbloqueadas coloridas e com a data, e um aviso
  pequeno de conquista desbloqueada durante o jogo.
- O renderizador nativo para o FH2: grama e folhagem, partículas, os faróis à noite e as cascatas de sombra do sol.
- Otimizações no caminho da GPU emulada: as faixas de tiling da cena em uma passada só, resolução abaixo de 1x,
  limite de MSAA, espera por futex no anel de comandos, afinidade de threads, vblank adaptativo.
- Correções: barreiras de memória reais no gerador de código no ARM, laço de eventos do SDL girando à toa no
  Android, reaproveitamento do cache de pipelines no Android.
- Ferramentas de medição: timestamps de GPU por tipo de trabalho, registro por quadro de resolves e cargas de
  textura, um profiler por amostragem e testes roteirizados.

Problemas conhecidos:

- Taxa de quadros baixa nos celulares, limitada pela emulação da GPU (o processamento de comandos nos celulares
  rápidos; os resolves e recargas de textura nos mais fracos).
- Os carros colidem com o cenário e entre si, mas a colisão ainda não está perfeita: no Android o carro às vezes
  ainda atravessa alguns objetos; em investigação.
- Na oficina de pintura, a imagem do carro no cartão Designs fica esticada.
- O renderizador nativo ainda tem erros de imagem em alguns lugares; o emulado é o fiel.
- Algumas opções do menu ainda não funcionam (o launcher as marca assim).

## Requisitos

**Para jogar no Android**

- Android 8.0 ou mais recente, processador ARM64.
- GPU com Vulkan 1.1 ou mais recente.
- 4 GB de RAM ou mais (recomendado 6 GB ou mais).
- Cerca de 8 GB livres para os arquivos do jogo.
- Sua própria cópia de **Forza Horizon 2 para Xbox 360**, edição USA (Title ID `4D530AA4`), extraída do seu
  próprio disco.

**Para compilar**

- Linux x86-64 (desenvolvido em Ubuntu/Pop!_OS), Clang 20, CMake 3.25 ou mais recente, Ninja, Python 3.8 ou mais
  recente, Git.
- Para Android: Java 17 ou mais recente, o Android SDK com o NDK 28.2.13676358 e o CMake 3.30.5.
- Cerca de 20 GB livres em disco e 12 GB de RAM ou mais (o código gerado é grande).

## Compilando

1. **Baixe as fontes de terceiros** do SDK (elas não ficam neste repositório):

   ```sh
   python3 tools/fetch_thirdparty.py
   ```

2. **Extraia a imagem do seu próprio disco** para `assets/game_root` (a pasta é ignorada pelo Git):

   ```sh
   python3 tools/fase1_extraer.py /caminho/para/o/seu/ForzaHorizon2.iso -o assets/game_root
   ```

3. **Compile para Linux.** A primeira compilação roda o gerador de código sobre `assets/game_root/default.xex` e
   escreve o C++ em `app/generated` (ignorada pelo Git); demora um pouco.

   ```sh
   cmake -S app -B app/out/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DREXSDK_DIR="$PWD/sdk"
   cmake --build app/out/linux
   tools/rodar_pc.sh
   ```

   **Regenere o código depois de baixar mudanças no `app/overrides.toml`** (ou em qualquer outro `app/*.toml`). O
   código gerado não fica no Git. Esta compilação para Linux o regenera quando esses arquivos mudam, mas a do
   Android (passo 4) não consegue rodar o gerador; então, se você só compila o APK, uma correção feita ali não
   chega a ele. Por exemplo, a correção do crash da oficina de pintura só declara o `setjmp` e o `longjmp` do jogo
   no `app/overrides.toml`; sem regenerar, o jogo continua fechando ao entrar na oficina. Rode de novo a
   compilação deste passo, ou só o gerador:

   ```sh
   REXGLUE=$PWD/sdk/out/linux-amd64/rexglue tools/codegen.sh app
   ```

4. **Compile o APK Android** (depois do passo 3, que produziu o código gerado):

   ```sh
   ./build_android.sh
   ```

   O APK fica em `android/app/build/outputs/apk/release/app-release.apk`. Ele é assinado com a sua chave de
   release se existir `~/.android-keys/fh2-recompiled/keystore.properties` (ou o arquivo indicado em
   `FH2_KEYSTORE_PROPERTIES`), com `storeFile`, `storePassword`, `keyAlias` e `keyPassword`; senão, com a chave
   de debug.

## Rodando no Android

1. Baixe o APK em [Releases](../../releases) e instale, ou instale a sua própria build com
   `adb install -r android/app/build/outputs/apk/release/app-release.apk`. Builds assinadas com chaves
   diferentes (a dos Releases e a sua) não se atualizam entre si: desinstale uma antes de instalar a outra.
2. Dê o jogo ao launcher: escolha a imagem do seu disco (.iso) em **Jogo e saves**, que é lida no lugar, ou copie
   os arquivos extraídos do jogo para `Armazenamento interno/jogos/ForzaHorizon2`, por exemplo com
   `adb push assets/game_root /sdcard/jogos/ForzaHorizon2`.
3. Conceda o acesso a arquivos que o launcher pedir e toque em **Jogar**.

Para começar: resolução de renderização em 50 % ou menos, suavização desligada, modo de apresentação FIFO.

**Renderizador nativo.** Ele precisa da biblioteca de shaders traduzidos, que é feita a partir dos arquivos do
próprio jogo e por isso não vem no APK. Gere-a no PC com `tools/xenos/biblioteca.sh` (depois do passo 2 de
[Compilando](#compilando)) e copie `app/out/shaders_fh2/fh2_shaders.nfsp` para junto dos arquivos do jogo no
celular (ou junto da .iso), por exemplo com `tools/enviar_biblioteca_android.sh`. Depois escolha **Nativo** em
**Gráficos**.

**Drivers de GPU.** **Desempenho > Driver da GPU** importa um pacote de driver do adrenotools (.zip) e o testa
com Vulkan antes de deixar escolhê-lo. Nos Adreno 6xx e 7xx um Turnip (Mesa) costuma ser mais rápido que o
driver do próprio celular.

**Teclado e mouse.** Um teclado e um mouse por Bluetooth ou USB são reconhecidos sozinhos, como um controle:
não há nada para ligar. O jogo os lê como um controle de Xbox 360, junto com qualquer controle conectado, num
layout de corrida: A/D viram, W acelera, S freia, Espaço é o freio de mão (A), o mouse e as setas viram a
câmera, IJKL é o direcional, Enter é o Start, Esc e Tab são o Back, C é o B, X é o X e Z é o Y. O mouse prende
o ponteiro na primeira vez que se mexe. Qualquer tecla pode ser trocada dentro do jogo com **F4** (a janela de
opções do próprio jogo, categoria *Input/Keybinds/Controller*). Os controles de toque saem de cena quando um
teclado ou um mouse é conectado ou usado e voltam quando eles saem ou no primeiro toque.

## Tutoriais

- [Tutorial — Build do Android e biblioteca de shaders (português)](TUTORIAL_SHADERS_PT-BR.md): clone, codegen, biblioteca `fh2_shaders.nfsp`, APK, instalação no Moto G9 Play (Adreno 610) e configuração do renderizador nativo com fallback sem descriptor indexing (método A), sem Turnip.
- [Tutorial — Android build and native shader library (English)](TUTORIAL_SHADERS_EN.md): mesmo guia em inglês.

## Estrutura do repositório

| Caminho | Conteúdo |
|---|---|
| `app/` | A aplicação do jogo: ganchos e correções (`app/src`), manifesto do gerador de código e configuração |
| `android/` | O aplicativo Android: launcher, atividade do jogo, controles de toque, ponte nativa |
| `sdk/` | O ReXGlue SDK com as mudanças deste projeto |
| `tools/` | Scripts de extração, medição, profiling e testes |
| `docs/` | Notas sobre o disco e os formatos do jogo |

## Contribuindo

Relatos de bugs e pull requests são bem-vindos. Por favor:

- nunca anexe nem aponte para arquivos do jogo, dumps, código gerado ou builds que não sejam as dos Releases
  oficiais;
- não peça arquivos do jogo nem builds: essas issues são fechadas;
- ao relatar um problema, descreva o aparelho (modelo, GPU, versão do Android) e anexe o arquivo de diagnóstico
  do launcher.

## Créditos

Este projeto existe graças ao trabalho de:

- **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)**, de Tom Clay: o recompilador e runtime sobre o qual
  tudo aqui é construído.
- **[Xenia](https://xenia.jp)**, de Ben Vanik e dos colaboradores do Xenia: a pesquisa sobre o Xbox 360 da qual
  derivam o kernel e a emulação de GPU do SDK.
- **[NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled)**, de madelrandel-blip: o projeto de
  recompilação do qual este partiu.
- **[nfsmw-nx](https://github.com/StevensND/nfsmw-nx)**, de StevensND, e
  **[nfsmw-android](https://github.com/codepdbh/nfsmw-android)**, de codepdbh: os ports cujas mudanças no SDK e
  cujo aplicativo Android foram adaptados aqui.
- **[NFSMW-Recompiled-Mobile](https://github.com/victorgbd/NFSMW-Recompiled-Mobile)**, de victorgbd: otimizações
  para celulares portadas para este projeto.
- Os autores das bibliotecas listadas em [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

O FH2 Recompiled é mantido por [Gabrick75](https://github.com/Gabrick75).

## Aspectos legais

- **Nenhum conteúdo do jogo no repositório.** Este repositório contém apenas código-fonte original e código
  aberto sob as licenças listadas em [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Ele não contém, e nunca
  conterá, o executável do jogo, arquivos de dados, assets, áudio, vídeo, artes, logotipos nem o código gerado a
  partir do jogo.
- **Releases.** Os APKs publicados nos Releases não contêm dados, assets, áudio, vídeo, artes nem logotipos do
  jogo, e não fazem nada sem os arquivos que o usuário extrai da própria cópia. São gratuitos e oferecidos
  apenas para donos do jogo.
- **Cópia própria obrigatória.** Você precisa ter uma cópia legítima do jogo e usar a sua própria extração dela.
  Este projeto não fornece cópias do jogo nem explica como obtê-las: ele funciona apenas com arquivos que o
  usuário extrai de um disco que possui.
- **Nenhum código da Microsoft.** Este projeto não inclui código do kit de desenvolvimento do Xbox nem de outro
  software proprietário da Microsoft.
- **Marcas registradas.** Forza, Forza Horizon, Xbox e Xbox 360 são marcas registradas do grupo de empresas
  Microsoft. Todas as outras marcas pertencem aos seus respectivos donos. Elas aparecem aqui apenas para
  identificar o jogo e a plataforma com os quais este software é compatível; nenhum endosso é sugerido.
- **Sem uso comercial nem pirataria.** Não venda este projeto nem builds dele, e não o use para distribuir ou
  jogar cópias não autorizadas do jogo.
- **Pedidos de remoção.** Se você é detentor de direitos e acredita que algo neste repositório ou nos seus
  Releases viola seus direitos, abra uma issue ou contate o mantenedor: o pedido será analisado e o conteúdo
  removido prontamente.
- **Sem garantia.** Este software é fornecido "como está", sem garantia de qualquer tipo, conforme as seções 15 e
  16 da GPL-3.0. Use por sua conta e risco.

## Licença

O FH2 Recompiled é software livre, licenciado sob a
[GNU General Public License v3.0](LICENSE). O ReXGlue SDK em `sdk/`, e as mudanças deste projeto nele, são
licenciados sob a [licença BSD-3-Clause](sdk/LICENSE). Componentes de terceiros mantêm as próprias licenças,
listadas em [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
