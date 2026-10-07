# Forza Horizon 2 (Xbox 360) — inventário do disco

Edição: **USA (En,Ja,Fr,De,Es,It,Pt,Zh,Pl,Ru)**, disco XGD3 (partição em 0x02080000), 1 de 1.
Extraída com `nfsmw-android/tools/fase1_extraer.py` para `assets/game_root` (7,3 GB, 2.988 arquivos).
Nenhum arquivo do jogo, nem código gerado a partir dele, entra no git.

## Executáveis

| Arquivo | SHA-256 | Base | Entry point | Imagem | Flags |
|---|---|---|---|---|---|
| `default.xex` | `516b04d7c69313cc581062e44030af3636f689e7f09eed1a709e4319f031feb7` | 0x82000000 | 0x82BF2CD0 | 24.117.248 B | 0x01 (título) |
| `XMediaFacade_default.xex` | `1b0801641622f04390ece6cdc0e2d1c0af3aee47cad59ec5c4999a3a3ea56490` | 0x88000000 | 0x88050800 | 2.818.048 B | 0x09 (DLL) |
| `SpeechFacade_default.xex` | `275ed7039b08f3d9bfb0fb690b641dc9d97323e73bcf7a4618c84c0306227878` | 0x89000000 | 0x890A0468 | 2.359.296 B | 0x09 (DLL) |

- Title ID `4D530AA4`, Media ID `7C7779D1`, versão 0.0.0.12. Os três importam só `xboxkrnl.exe` e `xam.xex`.
- Os dois "Facade" são DLLs carregadas em tempo de execução (`XexLoadImage`) → cada uma vira um módulo
  (`rexglue init module --xex-path ... --guest-path ...`). Os nomes e os arquivos `nuisp*`, `Nui*.bin.be`
  sugerem Kinect (voz e cabeça). Kinect é opcional no FH2; talvez dê para o jogo seguir sem carregá-las.
- Imagem principal ~1,8× a do NFSMW (13,4 MB) → esperar ~1,8× mais C++ gerado e mais tempo de build.

## Formatos de dados

| Extensão | Qtde | O que é |
|---|---|---|
| `.fsb` / `.fev` | 1.029 / 47 | FMOD (bancos de som / eventos). O FMOD é código do jogo, recompilado; o som chega como XMA via XAudio. |
| `.zip` | 678 | Pacotes de dados do ForzaTech (carros, pistas, UI). |
| `.fxobj` | 174 | **Shaders compilados em arquivos próprios** (`media/shaders`, 22 MB). No NFSMW eles vinham embutidos em contêineres; aqui a extração para o XenosRecomp deve ser mais simples. |
| `.wmv` | 19 | Vídeos (intros em `media/ui/videos/splash_intros`, cenas em `anthem/`). |
| `.xex` em `$SystemUpdate` | — | Atualização do dashboard; irrelevante para o porte. |

Maiores pastas: `media/tracks` 3,1 GB, `media/cars` 1,9 GB, `media/audio` 1,1 GB, `media/ui` 716 MB.
