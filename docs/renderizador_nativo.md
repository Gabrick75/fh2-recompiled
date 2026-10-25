# Renderizador nativo — mapeamento da biblioteca Direct3D do FH2

Primeiro passo do estudo de um renderizador nativo em Vulkan para o Forza Horizon 2, no estilo do
renderizador nativo do [NFSMW-Recompiled_ff](https://github.com/frankyfife/NFSMW-Recompiled_ff): interceptar as
chamadas da biblioteca Direct3D do Xbox que o jogo usa e desenhar sem emular a GPU Xenos.

## Ferramentas

Em `tools/renderprobe`, adaptadas do fork (GPL-3.0):

- `callgraph.py`: grafo de chamadas a partir de `app/generated/default/fh2_recomp.*.cpp`
  (85.237 funções, 212.879 chamadas diretas).
- `d3d_layer.py`: procura o bloco da biblioteca em volta das funções que chamam `Vd*`.
- `gen_census_fh2.py`: gera `app/src/fh2_censo.cpp`, wrappers que contam as chamadas por quadro de cada ponto de
  entrada da biblioteca (`--fh2_censo=true`, relatório `[censo]` a cada 150 quadros).

## Onde está a biblioteca

Ao contrário do NFSMW, a biblioteca do FH2 não é um bloco contíguo:

- **Parte baixa** (anel de comandos, swap, interrupções): `0x82B70DA0`–`0x82B81E60`, 322 funções.
  `sub_82B72118` chama `VdSwap` (uma vez por quadro); `sub_82B72A90` espera espaço no anel.
- **Parte alta** (device, estado, draws, tiling): no começo do código, `0x823F0000`–`0x82488000`, misturada com a
  biblioteca C (memcpy e afins, por isso 13 mil funções do jogo chamam essa faixa).
- **Device**: `0x4015D980` (o `r3` das funções mais chamadas).

## Funções identificadas

Pelas constantes dos cabeçalhos PM4 que montam (`lis`/`ori`) e pelo censo numa corrida (PC, 50 %, sem MSAA):

| Função | Por quadro | O que é |
|---|---|---|
| `sub_82426180` | 606 | draw (`DRAW_INDX`, predicado), `r4` = tipo de primitiva |
| `sub_82425D68` | 298 | draw (`DRAW_INDX`, predicado) |
| `sub_82418988` | 11 | draw (par de `sub_82426180`) |
| `sub_8241DA58` | 8 | draw (par de `sub_82425D68`) |
| `sub_82426800` | 487 | escreve `EVENT_WRITE` (0x46) |
| `sub_8244BFD8` | 517 | recebe 1280×720 (`r6`=0x500, `r7`=0x2D0): viewport ou superfície |
| `sub_82445C78`, `sub_82445B18`, `sub_824526B8` | 3894, 2449, 1402 | recebem o device: estado (a identificar) |
| `sub_82478390`, `sub_8247A868`, `sub_82478BC8` | — | tiling predicado: início da gravação, repetição, fim (já enganchadas) |
| `sub_82B72118` | 1 | swap |

## O achado que muda o plano

Contagem de pacotes PM4 por quadro na mesma corrida (`--gpu_medir_pacotes=true`):

| Pacote | Por quadro |
|---|---|
| `DRAW_INDX` (0x22) + `DRAW_INDX_2` (0x36) | 2122 + 226 = 2348 |
| `INDIRECT_BUFFER` (0x3F) | 650 |
| escritas de registradores (tipo 0) | 32754 |
| `IM_LOAD` (0x27, shaders) | 2040 |

As funções de draw da biblioteca somam **~920 draws por quadro; a GPU executa ~2350**. O resto (~60 %) vem de
**buffers de comandos pré-gravados**, chamados por `INDIRECT_BUFFER` (650 por quadro): o jogo grava listas de draws
e as repete, sem passar pelas funções de draw a cada quadro.

Consequência: interceptar só as chamadas de draw, como no NFSMW, perderia mais da metade da cena. Um
renderizador nativo do FH2 precisa também de um dos dois:

1. interceptar a gravação e a execução desses buffers na biblioteca (BeginCommandBuffer/RunCommandBuffer e as
   correções que o XDK aplica neles), guardando uma versão nativa de cada um; ou
2. interpretar os pacotes PM4 desses buffers (um processador de comandos próprio, sem a emulação da EDRAM).

## Quem executa os buffers pré-gravados

Os pacotes `INDIRECT_BUFFER` (cabeçalho `0xC0013F00`/`0xC0003F00`) são montados por `sub_824191B0` e por
**`sub_82426800`, chamada 487 vezes por quadro** com o device em `r3` e um objeto de buffer de comandos em `r4`: é
o `RunCommandBuffer` da biblioteca. Antes de chamar o buffer, ela grava o estado pendente do device, cruzando as
máscaras "sujas" do device (`+0`, `+8`, `+16`, `+24`, `+32`) com as máscaras do objeto (`+24` a `+56`):

| Grupo | Registrador Xenos | No device |
|---|---|---|
| Constantes float de vértice | `0x4000` | `+1920` |
| Constantes float de pixel | `0x4400` | `+6016` |
| Constantes bool/loop | `0x4900` | `+10112` |
| Estado de render | `0x2000`, `0x2080`, `0x2100`, `0x2180`, `0x2280`, `0x2380` | `+10368` em diante |

São as mesmas posições que o fork documentou no NFSMW (`docs/NATIVE_RENDERER.md` dele): é a mesma biblioteca do
XDK, e o mapeamento dele vale para o FH2. O objeto com tipo `10` no primeiro campo (`lwz r11,0(r4)`, 4 bits) segue
outro caminho (`sub_82B6EF10`).

## Os buffers pré-gravados: estáticos ou refeitos?

`--gpu_estudo_ib=true` registra, a cada 10 s, as execuções de `INDIRECT_BUFFER` com endereço, tamanho e hash do
conteúdo (PC, corrida, 50 %):

| Medida (por 10 s, na corrida) | Valor |
|---|---|
| Execuções | ~132 mil (~650 por quadro, ~930 dwords cada) |
| Buffers distintos | ~6 mil (cada um roda ~22 vezes) |
| Distintos que já existiam no período anterior | ~2,5 mil |
| Execuções com conteúdo mudado desde a anterior | 3,5–4,6 mil (~3 %) |
| Buffers conhecidos desde o início | sempre crescendo (73 mil em 2,5 min) |

Conclusão: os buffers são **criados pelo jogo enquanto ele roda** (endereços novos o tempo todo, provavelmente ao
carregar o cenário) e **reaproveitados por vários quadros**; uma parte pequena muda de conteúdo entre execuções
(as correções que o XDK aplica aos buffers antes de executá-los, ou regravações no mesmo lugar).

Para o renderizador nativo isso significa:
- um buffer pode ser decodificado uma vez e reaproveitado enquanto o conteúdo (hash) não mudar;
- o estado herdado do device no momento da execução (máscaras de `RunCommandBuffer`) entra em cada execução;
- as mudanças de conteúdo precisam ser detectadas (hash por execução, ~930 dwords: barato).

## A base escolhida: o renderizador nativo do nfsmw-nx (2026-10-08)

O APK "NFSMW Android Evolved 0.6.2" (30–40 fps no NFSMW num Unisoc T618 com 4 GB) declara a licença GPL-3.0 e é
construído sobre o **nfsmw-nx** (`github.com/StevensND/nfsmw-nx`, GPL-3.0, código aberto, porte do NFSMW para o
Switch). O renderizador nativo dele está em `app/src/nfsmw_nativo_*` (~26 mil linhas) e explicado em
`docs/native-renderer.md` daquele repositório. Como este projeto também é GPL-3.0, o código pode ser usado com crédito.

Por que ele serve ao FH2 (e o renderizador do NFSMW_ff, que intercepta as chamadas Direct3D, não):

- **Lê o anel PM4** (inclusive `INDIRECT_BUFFER`), em vez de interceptar as funções de draw: os buffers pré-gravados,
  ~60 % dos draws do FH2, entram sem trabalho extra.
- Não emula a EDRAM: render targets viram imagens Vulkan, resolves viram cópias (ou trocas de imagem quando o alvo vai
  ser limpo em seguida); só imagens pequenas que a CPU lê (exposição 64×64) voltam à memória do jogo.
- Shaders traduzidos **antes de jogar** pelo XenosRecomp (hedge-dev, MIT) a partir dos contêineres originais
  (cabeçalho, tabela de constantes, declaração de vértices, microcódigo), compilados com DXC para SPIR-V e guardados
  numa biblioteca (`.nfsp`) achada pelo hash do microcódigo. Vertex shaders são modificados pelo D3D ao serem usados, então
  a identidade deles vem de ganchos nas funções que criam shaders.
- Três slots de trabalho por quadro, constantes num uniform buffer dinâmico, cache de texturas por hash com checagem cada
  vez mais rara, cache de pipelines em disco e pré-aquecido.

O que o FH2 tem para isso: **174 `.fxobj` em `media/shaders` com 2.921 contêineres** no formato de 2008 (`10 2A 11 00/01`),
o que o XenosRecomp lê sem o conversor de formato que o NFSMW (formato de 2005, `10 2A 0E`) precisou.

O desempenho do Evolved também vem de cortes específicos do NFSMW (`assets/nativo/nfsmw.toml` do APK): reflexo atualizado a
cada 4 quadros e só quando visível, sombras cortadas a 150 m num mapa só, PCF barato, sem desfoque, sem MSAA, Z
antecipado, invisíveis pulados, 720p. No FH2 os equivalentes precisam ser achados no código do jogo.

### Etapa 1, primeira medida: a biblioteca de shaders (`tools/xenos/biblioteca.sh`)

Fontes dos contêineres:
- `media/shaders/**/*.fxobj`, soltos;
- 99 `.fxobj` distintos nos zips das pistas (`media/tracks`).

Os zips das pistas (até 3 GB) não têm cabeçalhos locais: os dados de cada entrada começam no `header_offset`. O método
21 é o LZX do XMemCompress, em quadros `[FF, descomprimido BE16, comprimido BE16]` ou `[comprimido BE16]`, que juntos
formam um fluxo LZX com janela de 128 KB.

| Medida | Valor |
|---|---|
| Contêineres | 3.587 |
| Traduzidos para HLSL (XenosRecomp do nfsmw-nx) | 3.132 |
| SPIR-V válido (DXC 1.9 + spirv-val) | 2.559 |
| Recusados na tradução: busca de vértices sem elemento declarado | 455 vertex shaders |
| Recusados pelo DXC: `oNormal0`/`iNormal0`/`oBinormal0`/`oTangent0` não declarados, `getWeights2D`, `cubeMapData` | 573 |

Cobertura dos shaders que o jogo carregou no PC (armazenamento `.xsh` do emulador; vertex shaders comparados sem as
instruções de busca, que o D3D modifica):

| | Usados | Na biblioteca | Já em SPIR-V |
|---|---|---|---|
| Pixel shaders | 325 | 228 (70 %) | 154 (47 %) |
| Vertex shaders | 288 | 170 (59 %) | 85 (30 %) |

Falta:
- corrigir o tradutor para os padrões do FH2 (normais, tangentes e binormais como entrada e saída, `getWeights2D`,
  cubemaps, buscas de vértices sem elemento declarado);
- achar os ~100 pixel e ~120 vertex shaders que não estão nos `.fxobj`. São pequenos (15–80 dwords): prováveis shaders
  internos da biblioteca D3D (limpeza, resolve, cópia) e de efeitos dentro do executável ou de outros pacotes.

### Etapa 1, resultado (2026-10-08)

Fontes extras de contêineres:
- o executável já carregado (`--xex_despejar_imagem`): 482 contêineres no módulo principal e 68 no segundo;
- shaders internos do D3D, sem tabela de constantes.

Mudanças do tradutor do FH2 (`FH2_RECOMP` em `tools/xenos/recomp`):
- **interpoladores:** `NORMAL0`, `TANGENT0`, `BINORMAL0` e `POSITION1`;
- **samplers:** área própria por slot de busca, com os de vértice em 16–19;
- **buscas de vértice sem elemento** (dados por instância, constantes 88–95): lidas direto de um storage buffer;
- **constantes:** 256 de pixel; as sem nome lidas das constantes do jogo; constantes de laço numa área compartilhada;
- **elementos repetidos** declarados uma vez só;
- **texturas 1D** tratadas como 2D de altura 1.

Cobertura dos shaders carregados no PC: **pixel 324 de 325 encontrados, todos compilam; vertex 235 de 288 encontrados,
todos compilam.** Os vertex shaders que faltam devem ser os que a biblioteca D3D modifica além do que o mascaramento
simples de `cobertura.py` reconhece. O renderizador os identifica pelos ganchos e pela comparação tolerante do Carbon.

### Base do renderizador: nfsmw-android atual e o porte para o NFS Carbon

O `codepdbh/nfsmw-android` atual (GPL-3.0; clone em `referencias/nfsmw-android-atual`) já traz o renderizador do
nfsmw-nx adaptado ao Android. Na prática é o código do "Evolved". O `codepdbh/NFS-CARBON-360-ANDROID-RECOMP`
(`referencias/nfs-carbon`, `docs/renderizador-nativo.md`) levou esse renderizador a outro jogo:
- **Compilação:** a partir de `nfsmw-android/app/src`, com o define `NFSC_RECOMP`.
- **Ganchos do jogo:** cerca de 95 linhas, cobrindo `CreateVertexShader` e `CreatePixelShader`, as funções `Draw*` e
  os deslocamentos do VS e do PS no device.
- **"Enlaces":** respostas neutras ao que o renderizador pede dos módulos próprios do NFSMW.
- **Como acharam as funções:** comparando o código gerado dos dois jogos (impressão digital das instruções e grafo de
  chamadas), porque a biblioteca D3D do XDK é a mesma.
- **Resultado:** imagem completa a ~60 fps em 720p num S25.

O FH2 segue o mesmo caminho, com `FH2_RECOMP`.

### Funções D3D do FH2 que o renderizador pede (como o Carbon)

Sem o código gerado do NFSMW para comparar, achadas em tempo de execução com o censo (`--fh2_censo=true`,
`tools/renderprobe/gen_census_fh2.py`). O censo agora também registra:
- as funções que recebem um contêiner de shader em `r3`–`r5`;
- as que recebem em `r4`/`r5` um objeto devolvido pelos construtores.

| Papel | FH2 | Carbon | Como se confirmou |
|---|---|---|---|
| `CreatePixelShader` (`r3` = contêiner, devolve o objeto) | `sub_82B70280` | `826E4218` | 511 contêineres de pixel |
| `CreateVertexShader` | `sub_82B70468` | `826E4680` | 412 de vértice; copia a parte virtual para objeto+872, como no Carbon |
| `SetVertexShader` (`r4` = objeto) | `sub_8245B370` | `826E4790` | `stw r29,13072(r30)`: VS em **device+0x3310** |
| `SetPixelShader` | `sub_8245BA10` | `826E4350` | `stw r29,13068(r30)`: PS em **device+0x330C** |
| Draws (`DRAW_INDX`) | `sub_82426180`, `sub_82425D68`, `sub_82418988`, `sub_8241DA58` | `826DDA78`… | mapeados antes; falta dizer qual `Draw*` é cada um |
| `RunCommandBuffer` | `sub_82426800` | — | |
| Swap (chama `VdSwap`) | `sub_82B72118` | `826E27A8`? | |
| Calcula tamanho ou valida contêiner (os dois tipos) | `sub_82B70108` | | |

### Etapa 2, estado (2026-10-08)

O renderizador do nfsmw-android está em `app/src/nativo` (`FH2_RECOMP`) e é ligado com `--nfsmw_renderizador=nativo`
(`tools/xenos/biblioteca.sh` gera e copia `fh2_shaders.nfsp`). No PC, no menu de eventos:

- Anel, interrupções e Swaps funcionam (~60 fps no PC).
- Shaders:
  - pixel shaders identificados pelo microcódigo;
  - vertex shaders pelo microcódigo ou pelo endereço da cópia que o parcheador `sub_8241ACA0` escreveu
    (`Fh2AnotarParcheVs`; 6.669 acertos, 1 falha).
- Resolves aceitam os formatos do FH2: `2_10_10_10`, `16_16_16_16`, `16_16_16_16_FLOAT`, `16_16_FLOAT` e `32_FLOAT`.
  O viés de expoente das cópias ainda não é aplicado.
- Entradas de vértice `10_11_11` (posições e coordenadas de textura): ligadas como `R32_SFLOAT` e decodificadas no
  shader (`fh2Entrada`, constante de especialização 1).
- **Resultado:** a interface do menu aparece certa; a cena 3D ainda fica preta.

Recusas que sobram por 40 s: 1.404 draws pela causa 12 e 396 por tipo de primitiva (101). Também 1.938 cópias com
retângulo vazio e 62 Swaps sem textura resolvida.

Próximo: achar onde a cena se perde, entre o alvo HDR (`2_10_10_10_FLOAT` e a vista `AS_16_16_16_16`, que o Carbon
precisou unir), o viés de expoente e a composição final. O mosaico de diagnóstico (`nfsmw_nativo_diag_resueltas`) só
mostra texturas 8888.

### Etapa 2, estado (2026-10-09, madrugada)

No PC, com `--nfsmw_renderizador=nativo`:
- o menu de eventos sai como na emulação, com o fundo 3D desfocado;
- **a corrida renderiza**: carro, cenário e interface completa.

Correções do caminho até aqui, além das anteriores:
- **Texturas:** pixel shaders no layout do NFSMW.
- **Tiling:** faixa única no modo nativo, com as regras do `gpu_ignorar_predicacao`; cópias de faixa vão para a textura
  da base, e as limpezas de faixa são adiadas até a última.
- **Shaders internos do D3D:** contêineres sintéticos e biblioteca sem tabela de constantes.
- **Vertex shaders:** os copiados para o anel são identificados por endereço ou conteúdo; os de esqueleto e de
  instancing leem os buffers diretamente (storage buffer no set 4, parâmetros modificados pelo D3D por elemento).
- **Listas de retângulos:** 4º canto em formatos de 16 e 8 bits.
- **Amostragem:** coordenadas de textura não normalizadas; samplers tirados do microcódigo.

Falta, na corrida:
- imagem estourada: viés de expoente das cópias (−2 na cena, −4 nos desfoques) ainda não aplicado;
- rastro forte de desfoque e granulado em muros e árvores;
- faixa verde na borda;
- desempenho no PC, hoje com 60–100 ms por quadro.

### Etapa 2, estado (2026-10-11)

No PC, a corrida roda a 30 fps (o teto do jogo), com 30–36 ms por quadro. Corrigido desde a madrugada de 09/10:
- **Culling:** listas de retângulos nunca são descartadas.
- **Texturas resolvidas:** as substituídas por outro formato no mesmo endereço vão para um "cemitério", em vez de esperar a
  GPU.
- **Leituras de volta:** chegam à memória do jogo antes das cercas.
- **Buffers de ossos e instâncias:** vigiados por escrita de página.
- **Texturas com gama:** lidas em sRGB.
- **Samplers de vertex shader:** associados.
- **Cubemap do reflexo:** montado com faces 2_10_10_10.
- **Cascatas de sombra:** cada uma na própria textura.

Depois, no mesmo dia:
- **Limpezas MSAA 4x de meio pitch:** vão para a superfície 1x. Assim o pré-passe de profundidade é zerado e as sombras
  aparecem.
- **`exp_adjust` dos fetches de textura:** aplicado. A exposição da corrida ficou igual à da emulação.
- **Shader de limpeza do D3D:** os contêineres sintéticos agora declaram interpoladores. Isso tirou o brilho verde nas
  bordas.
- **8_8_8_8_GAMMA:** usa a mesma imagem de 8_8_8_8. Os vídeos da abertura e do título aparecem.

Com isso, abertura, título, carregamento, menu e corrida saem no PC como na emulação, a 30 fps.

**Android:** no Moto G9 Play (Adreno 610, driver Vulkan 1.1.128 da Qualcomm) o renderizador nativo não desenha. Falta
`VK_EXT_descriptor_indexing`, de que ele precisa para as tabelas de texturas. Caminhos possíveis: carregar o Turnip
(libadrenotools; o launcher já importa drivers) ou um modo sem descriptor indexing.

### Diferenças visuais contra a emulação (2026-10-12)

Comparação no PC, mesma cena (save do S23, noite na costa), traço de um quadro nos dois renderizadores
(`--nfsmw_nativo_diag_fotograma_s` e `--gpu_traco_quadro_s`, `tools/comparar_trazas.py`):

- **Grama ausente (corrigido).** Os vertex shaders do FH2 começavam com `r0 = 0`: o XenosRecomp só declarava
  `SV_VertexID` para o Unleashed. O Xenos põe o índice do vértice em `r0.x`, e os shaders da grama (v_003789 e
  parecidos) montam o quad de cada tufo a partir dele (instância = índice / 4). Agora `r0.x = SV_VertexID -
  FH2_BASE_VERTICE`, onde a constante compartilhada `FH2_COMPARTIDA_BASE_VERTICE` (byte 1236) é o deslocamento que
  o draw aplicou (`vertexOffset`/`firstVertex`: `primer_vertice - vmin` no caminho base-zero, `-vmin` no outro).
  **Exige regerar a biblioteca** (`tools/xenos/biblioteca.sh`) e enviá-la ao celular.
- **Estrelas (corrigido).** As listas de pontos (tipo 1; o céu noturno, VS n1292, 9096 pontos de 1 px) eram
  recusadas. Agora são `VK_PRIMITIVE_TOPOLOGY_POINT_LIST`, e os vertex shaders escrevem `PointSize`
  (struct `Fh2Punto`; o DXC não aceita o builtin num parâmetro `out` simples) a partir de
  `FH2_COMPARTIDA_TAM_PUNTO` (byte 1240, de `PA_SU_POINT_SIZE`). Tamanho por vértice não é suportado.
- **Câmera interna cinza (corrigido).** O jogo desenha o habitáculo marcando o stencil, depois uma lista de
  retângulos em modo só-profundidade que empurra a profundidade para perto onde o stencil ≠ 0
  (`RB_DEPTHCONTROL 087005F7`: NOTEQUAL na frente, ALWAYS na face de trás), depois o mundo com GEQUAL, que só passa
  pelos vidros. Como dois triângulos, os retângulos podiam ficar de costas e pegavam o ALWAYS da face de trás.
  Listas de retângulos, pontos e linhas não são polígonos: como na emulação, `backface_enable` é ignorado nelas.
- **Faróis sem cone de luz no chão (aberto).** Os volumes de luz por stencil (VS n2599 + quad VS n1196) existem
  no nativo e desligar o stencil do quad não mostra nada: o cone dos faróis deve vir de outro caminho (provavelmente
  dentro dos shaders do chão/grama). Na comparação o carro também sai iluminado de cima, diferente da emulação.
- **Pistas que não eram a causa:** a primeira comparação sugeriu 180 draws de vegetação "faltando" no nativo; era
  variação entre quadros (o carro do emulado andava). Os filtros do NFSMW (`nfsmw_d3d_vegetacion_juego`: nem é
  chamado no FH2; `nfsmw_sombras_sin_vegetacion`, `ps_solo_alfa`, `saltar_invisibles`), o limiar do
  alpha-to-coverage, as consultas de oclusão, as leituras de volta e o modo de uma faixa foram testados sem efeito.
- **Depuração nova:** `fh2_nativo_diag_omitir_vs` (pula os draws de VS dados), `fh2_nativo_diag_sin_stencil_vs`,
  `fh2_censo` com cadeias de chamadores dos draws, e os traços listam os buffers indiretos.

### Reflexo do carro e definição (2026-10-20)

Comparação no PC, save do S23 no mato (`app/out/save_s23_mato`), carro parado, captura aos 90 s nos dois
renderizadores, tudo no padrão e tudo no máximo:

- **Pintura lavada, quase branca, e vidro sem as árvores (corrigido).** A direção da busca no cubo vinha do atalho
  do Unleashed: `cube()` guardava o `xyz` do registro e o `tfetchCube` usava essa direção. O Xenos lê a direção
  do operando 0 como `(z, _, x, y)`, com swizzle e modificadores (`spirv_translator_alu.cpp`, kCube). Os shaders do
  Unleashed sempre passam `v.zzxy`; a pintura do FH2 (PS n1939, p_002583) passa `r6.xxzy`, ou seja, a direção é
  `r6.zyx`. Com X e Z trocados, o carro refletia o céu e o sol onde estão as árvores. Cerca de 240 das ~940
  instruções `cube` da biblioteca usam swizzles fora do padrão. O tradutor agora monta a direção a partir do
  operando 0. **Exige regerar a biblioteca** (`tools/xenos/biblioteca.sh`) e enviá-la ao celular. As faces do
  cubo já saíam iguais às da emulação (comparadas com `fh2_nativo_diag_volcar_resueltas` contra o
  `traco_despejo` do emulado).
- **Mips do cubo do reflexo (corrigido, `fh2_nativo_mips_cubo`).** O jogo reduz cada face até 1x1 na região de mips
  do fetch, mas o cubo nativo era montado só com o nível base. Agora ele leva os 9 níveis.
- **Pintura facetada no Adreno com os mips do cubo (corrigido, 2026-10-25).** Na 0.4.1 o S23 mostrava a malha do
  carro: cada triângulo da carroceria com um tom constante. Com `fh2_nativo_mips_cubo=false` a pintura ficava lisa,
  e no PC saía lisa com os mips. Ou seja, o nível implícito do `Sample` no cubo era escolhido errado por triângulo
  no Adreno. Os mips em si estavam certos (faces, orientação e brilho comparados nível a nível nos dumps de
  `fh2_nativo_diag_volcar_resueltas`). Agora `cube()` guarda as derivadas da direção, e `tfetchCube` calcula o nível
  com a regra de cubo do Vulkan (derivadas das coordenadas da face principal) e amostra com `SampleLevel`. No PC a
  imagem é a mesma. Os VS continuam no nível 0. Exige regerar a biblioteca.
- **Viés de mip do fetch (corrigido, `fh2_nativo_sesgo_mip`).** O jogo pede −0,375 a −3 em texturas que quer
  nítidas, e o sampler nativo ignorava. Agora vai em `mipLodBias` (limitado a `maxSamplerLodBias`), como na
  emulação.
- **Árvores sem sombra (opção).** `nfsmw_sombras_sin_vegetacion` (herdado do NFSMW, ligado) tira do mapa de sombras
  tudo o que tem prova de alfa. No PC, desligá-lo custou o passe de cena de ~2 para ~3,9 ms. O launcher o desliga
  com "Distância das sombras" em 1x ou mais (árvores com sombra, como no Xbox 360); abaixo de 1x ele fica ligado.
- **Ainda diferente:** o céu da emulação sai mais branco e enevoado, e a cena mais clara (brilho médio 121 contra
  107). A textura do chão e o grão das folhas já são iguais; a diferença de contraste na copa vem do céu atrás
  das folhas. `nfsmw_resplandor_cielo` não muda nada no FH2.
- **O tradutor ainda ignora** `setTexLOD`, o viés da instrução e o LOD explícito: 546 buscas em pixel shaders usam
  LOD de registro. As buscas no cubo da pintura não usam (LOD calculado).
- **MSAA que não suavizava (corrigido, `fh2_nativo_msaa_persistente`).** Com MSAA 4x, as bordas do nativo saíam tão
  serrilhadas quanto sem MSAA. As imagens multiamostra eram transitórias (`storeOp DONT_CARE`), e só os passes
  que o jogo começa limpando entravam em MSAA. Todo passe que continua a cena caía para 1x: no mato, 20.958
  passes multiamostra contra 44.622 a 1x. Agora as imagens multiamostra são persistentes (memória comum,
  `storeOp STORE`, `loadOp LOAD`), com uma marca `ms_valida` por alvo:
  - um passe MSAA deixa a imagem válida;
  - um passe 1x a invalida;
  - as limpezas fora de passe (`Fh2BorrarMs`) são aplicadas também à imagem multiamostra;
  - troca de imagens e restauração mantêm a marca, porque não mudam o conteúdo do alvo.

  Resultado na mesma cena: 31.820 passes multiamostra (21.391 continuando a cena) contra 2.898 a 1x. Custo: a
  imagem multiamostra ocupa memória de verdade (alvo de cena de 1280x1280 a 4x: ~52 MB só de cor), e cada passe
  grava as amostras. `false` volta ao comportamento anterior.
- **Alpha-to-coverage real nos passes MSAA (opção, `fh2_nativo_msaa_a2c`, desligada).** Com
  `alphaToCoverageEnable` no lugar do teste de alfa, as moitas ficavam macias no PC, mas no celular, a 2x e a 4x,
  viravam pontinhos de cobertura espalhados ("farinha"). Por padrão fica o teste de alfa, como antes.
- **FXAA do launcher nunca ligava (corrigido).** `nfsmw::ajustes::VigilarAjustesEnVivo()`, que copia
  `nfsmw_antialiasing` (e o pós-processo opcional) para os atômicos que a passada de saída lê, só era chamada no app
  do NFSMW; o `fh2_app.h` não a chamava. Agora é chamada ao criar o renderizador nativo, e o log mostra
  `antialiasing en la salida: FXAA`. O resplendor do céu e o tratamento visual que ela também liga dependem de
  hashes de shaders do NFSMW e não mudam nada no FH2.

### A biblioteca no celular (decidido, ainda não implementado)

`fh2_shaders.nfsp` é feito a partir dos arquivos do jogo de cada um, então não pode ir no APK. Decisão do dono do
projeto: **gerar no próprio celular**, a partir dos arquivos do jogo que o jogador copiou. Por enquanto ela é gerada no PC
(`tools/xenos/biblioteca.sh`) e enviada por `tools/enviar_biblioteca_android.sh`.

O que a geração no celular precisa:
- **Extração:** os passos de `tools/xenos/biblioteca.sh` em código nativo do app: descompactar os `.fxobj` das pistas
  (LZX), extrair os contêineres do executável e das pistas, e montar os contêineres sintéticos dos shaders internos do
  D3D.
- **Tradução:** o XenosRecomp (`tools/xenos/recomp`) compilado para Android.
- **HLSL → SPIR-V:** o DXC não tem build oficial para Android e é pesado. Alternativas: glslang com entrada HLSL
  (verificar se compila o HLSL gerado, que usa `vk::RawBufferLoad` e recursos de HLSL 2021), ou levar o DXC para o NDK.
- **Pacote:** o empacotador (`tools/xenos/empaquetar.cpp`, já em C++).
- **Tempo e espaço:** ~3700 shaders. No PC leva alguns minutos; no celular, rodar em segundo plano com progresso no
  launcher e só uma vez por versão da biblioteca. São ~72 MB de saída.
- **Os shaders internos do D3D:** hoje vêm de um despejo feito pelo próprio renderizador em execução
  (`--fh2_nativo_volcar_sueltos`). No celular isso exige uma primeira execução, ou levar esses microcódigos para dentro do
  app, se a licença permitir.

### Etapas

1. Biblioteca de shaders do FH2: XenosRecomp + DXC no PC sobre os `.fxobj` (e o XEX); medir quantos traduzem e quantos
   microcódigos do `IM_LOAD` da corrida são achados nela.
2. Portar o leitor do anel e o caminho de draws (`nfsmw_nativo_sistema`, `_dibujos`, `_destinos`, `_texturas_pool`,
   `_shaders`) para o FH2, ligado por cvar, desenhando a tela de título no PC.
3. Identidade dos vertex shaders (ganchos na biblioteca D3D do FH2), corrida no PC, comparação com a emulação.
4. Android (S23, Moto), depois cortes específicos do FH2.

## Próximos passos

- Achar as funções de gravação e execução dos buffers pré-gravados (quem monta os `INDIRECT_BUFFER`) e medir
  quantos buffers distintos existem e se mudam entre quadros.
- Identificar as funções de estado (render target, texturas, shaders, streams, constantes) pelos argumentos.
- Decidir entre as opções 1 e 2 acima.
