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

## Próximos passos

- Achar as funções de gravação e execução dos buffers pré-gravados (quem monta os `INDIRECT_BUFFER`) e medir
  quantos buffers distintos existem e se mudam entre quadros.
- Identificar as funções de estado (render target, texturas, shaders, streams, constantes) pelos argumentos.
- Decidir entre as opções 1 e 2 acima.
