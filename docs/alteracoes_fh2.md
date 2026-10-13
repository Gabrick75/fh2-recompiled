# Alterações — FH2 (Android e PC)

Registro vivo das mudanças feitas nesta linha de trabalho. Serve para o usuário pedir
"mostre o arquivo" e encaminhar o conteúdo à outra IA continuar de onde paramos.

Regra: cada alteração entra como um item com **estado**, **arquivos** (com `arquivo:linha`
quando ajudar), **o que mudou** e **por quê**. Nada é removido; itens concluídos ficam marcados.

Estados: `[pendente]` · `[em andamento]` · `[feito]` · `[descartado]` · `[medido]`

---

## 2026-10-11 — Início desta linha de trabalho

Contexto: o outro agente já tinha deixado o renderizador nativo em 30 fps no mundo aberto
(envio parcial a cada 250 draws) e reduzido a área aberta dos passes em ~35%. A partir daqui as
edições são feitas com assistência nesta sessão.

### [feito] Save do S23U copiado para o PC, para rodar o mesmo estado
- **Origem**: S23U (SM-S918B, serial `RQCXB01R9LM`), backup automático
  `/sdcard/jogos/ForzaHorizon2-saves/2026-10-11_20-50-11/`.
- **Destino**: `app/out/run/user/` (`B13EBABEBABEBABE/` + `achievements/`), que é o
  `--user_data_root` usado por `tools/rodar_pc.sh`.
- **Save anterior do PC**: guardado em `/tmp/opencode/pc_save_antes/`.
- **Verificação**: md5 iguais aos do S23U em `ForzaProfile`, `PlayerDatabase`, `GameplayLog`,
  `VersionFlags`, `Thumbnails/Thumbnail_2.xdc` e `achievements/4D530AA4.toml`.
- **Por quê**: rodar no PC o mesmo progresso/conquistas que no celular, para comparar cenários.

### [feito] Patch WIP do cache de vértices movido para um branch do git
- **Antes**: solto em `app/out/patches/cache_vertices_wip.patch` (fora do git).
- **Agora**: branch `wip/cache-vertices`, commit `3d70e42`
  ("Native renderer: WIP vertex cache (persistent GPU vertex buffer)").
- **Arquivos**: `app/src/nativo/nfsmw_nativo_destinos.cpp`,
  `app/src/nativo/nfsmw_nativo_dibujos.cpp`, `app/src/nativo/nfsmw_nativo_dibujos.h`
  (+254, −1).
- **Estado**: não vai para `main`; tem defeito de imagem conhecido (vértices errados ao reusar
  uma faixa). `main` segue em `025b876`, inalterado. O arquivo solto foi removido.
- **Por quê**: manter o trabalho versionado e recuperável, sem patch solto no diretório de saída.

### [medido] Escala da CPU do anel por draw (achado que muda o plano seguinte)
- Log: `app/out/logs_s23_au/fh2_20261011_205011.log`.
- Relatório `[fh2 nativo] anillo por Swap (ms): ocupado = espera a la GPU + espera al juego
  (WAIT_REG_MEM) + envios parciales + CPU` é a decomposição correta (as esperas ficam **dentro**
  do ocupado; não somar com `trabajando`).
- Três pontos do mundo aberto: 1330 draws → CPU 21,5 ms; 1673 → 22,6; 1864 → 22,7.
  Isto é **~18,5 ms fixo + ~2,3 µs/draw**, não 16 µs/draw. A projeção "2600 draws × 16 µs = 40 ms"
  não se sustenta; pela tendência, ~24 ms.
- **Consequência**: antes de "cortar CPU por draw / dividir a thread", confirmar no cenário pesado
  qual limita: CPU fixa, CPU/draw, GPU (`GPU real por Swap`) ou alimentação do jogo (WAIT_REG_MEM).

### [medido] Relatório de GPU por categoria (Adreno) é não confiável
- No mesmo trecho pesado (`app/out/logs_s23_ext/fh2_20261011_200051.log`): `escena` com
  805 draws / 206 k triângulos / 34 Mtexels reportou 2,0 ms; `menores` com 52 draws / 1 k triângulos /
  5,8 Mtexels reportou 20–21 ms. Ou seja, erro de ~10×.
- **Consequência**: usar só `GPU real por Swap` e o `GPU %`; ignorar `GPU por Swap` por categoria.

### [pendente] Instrumentar/validar a escala da CPU do anel no cenário pesado
- Medir no log pesado: `CPU` do anel vs `dibujos por Swap`, junto com `GPU real por Swap` e
  `espera al juego`. Decidir entre dividir a thread do anel ou atacar a GPU/área.

### [descartado] Adiar as cópias de resolve até o fim natural do passe
- Ideia: hoje ~70 passes/frame são reabertos após uma cópia (`C6 pases: ... reanudados tras copia o
  borrado`), forçando `vkCmdEnd/BeginRenderPass`; agrupar as cópias evitaria reabrir o mesmo alvo.
- **Investigação (2026-10-11)**: o adiamento **já existe** para os três casos:
  - Profundidade: `nfsmw_nativo_profundidad_perezosa` (`nfsmw_nativo_destinos.cpp:306`, default
    **true**), fila `pendientes_` + `GrabarCopiaPendiente` (`:3796`).
  - Front (o que o Swap lê): `nfsmw_nativo_frontal_perezoso` (`:338`, default **true**),
    `AplazarCopiaFrontal` + `GrabarCopiaFrontal` (`:3919`).
  - Composta: `nfsmw_nativo_compuesta_perezosa` (`:510`, default false), `AplazarCompuesta` (`:2674`).
  - O caminho de cor já tenta os dois antes da cópia imediata: `!AplazarCopiaFrontal(...) &&
    !AplazarCompuesta(...)` (`:1869`). Além disso o caso inteiro vira swap sem cópia
    (`IntercambiarConResuelta`, `:1817-1824`).
- **Já medido como ~0 ganho**: `AplazarCompuesta` registrou **19 895 de 21 041** cópias adiadas
  mesmo assim (um draw lê a textura quase todo frame) → 0 ms poupados (`:508-514`); o diagnóstico
  que o alimenta (`nfsmw_nativo_diag_lectores_s`, `:488`) foi desligado porque "só serviu à cópia
  composta preguiçosa, que não poupa nada".
- **Confirmado por medição nova (ver item abaixo)**: os reanudados **não são** cópias de resolve;
  são o resolve de cor que vem com o clear. Agrupar cópias não corta nada.

### [medido] Instrumentação da causa dos reanudados de passe (C6)
- **Arquivos**: `app/src/nativo/nfsmw_nativo_dibujos.h` (enum `kCierre*`, campo
  `pases_reanudados_motivo` no `EstadisticasDibujos`, método virtual `AnotarMotivoCierre`),
  `app/src/nativo/nfsmw_nativo_dibujos.cpp` (contador por motivo no ramo de reabertura ~`:3507`,
  assinatura/consumo do motivo), `app/src/nativo/nfsmw_nativo_destinos.cpp` (`Copiar`, `TexturaResuelta`
  e `AnotarLecturaProfundidad`/`Frontal` etiquetam o motivo), `app/src/nativo/nfsmw_nativo_sistema.cpp`
  (nova linha de relatório `C6 reanudados por causa`).
- **Saída nova**: `[nativo] C6 reanudados por causa: <motivo> <n>, ...` com motivos
  outro / copia color / borrado color / profundidad / intercambio / diferido / lectura resuelta /
  copia+borrado color.
- **Medição (PC, `nfsmw_renderizador=nativo`, teste de jogabilidade)**:
  - `cop by+borrado color 38768`, `profundidad 78`, `otro 79` — num intervalo de 20 s
    (~38 000 reanudados ≈ 60-65/frame). As linhas `copia color`, `borrado color`, `intercambio`,
    `diferido` e `lectura resuelta` saíram **vazias**.
  - **Conclusão**: ~100 % dos reanudados são o **resolve de cor que vem com `borrar_color`**
    (ver `destinos.cpp:1806-1810`: o clear é obrigatório no resolve de cor do FH2). O passe fecha
    para copiar+limpar e reabre porque o jogo volta a desenhar no mesmo alvo. Não é cópia de resolve
    adiável; adiar não evita o fechamento (o clear também exige sair do passe).
- **Alavanca real** (para depois): evitar a cópia nesse caminho (swap/restore com mais liberdade, ex.
  afrouxar a condição de `IntercambiarConResuelta` em `:1817`), não adiar cópias.

### [medido] Confirmação no S23 (corrida pesada) da causa dos reanudados
- **Aparelho**: S23U `RQCXB01R9LM`, log novo `app/out/logs_s23_reanudados/fh2_20261011_214035.log`
  (renderer=native, ~20-30 fps na corrida pesada).
- `C6 reanudados por causa` em todos os intervalos: **`copia+borrado color` 28 000-36 000** por 20 s
  (~50-73/frame); `otro` 1-13, `profundidad` 12-84, `copia color` 0-3. `borrado color` puro,
  `intercambio`, `diferido` e `lectura resuelta` **vazios**. Mesma conclusão do PC, agora no aparelho.
- `[tiron]` por fotograma: **~86 copias, ~80 borrados, 0 intercambios, 0 restauraciones** → ~80 resolves
  de cor com clear por frame e **o swap de cor não está sendo usado** (cvar
  `nfsmw_nativo_intercambiar_color`, `destinos.cpp:143`, default **false**).
- Timing pesado: anel `ocupado 40-48 ms = espera GPU 8-11 + WAIT_REG_MEM 5-13 + envios 0.5 + CPU do
  anel 26-32 ms`; `GPU real por Swap 31-39 ms`; uso do top-thread "anillo nativo" 47-67 % e GPU 60-83 %.
  CPU da anel **e** GPU perto do orçamento (co-limite). `vkCmdBegin/EndRenderPass` ~433 ms/20 s ≈ 0,9 ms/frame.
- **Contexto do `intercambiar_color`**: está off porque na NFSMW falhou (`destinos.cpp:130-137`): sem
  exigir clear, 16 807 swaps contra 17 350 restores (o jogo redesenha por cima sem limpar). Mas a medição
  do FH2 mostra que o resolve de cor **quase sempre vem com `borrar_color`** — que é a condição que o
  mecanismo já exige (`:1817`). Hipótese a testar: no FH2 o swap pode valer onde falhou no NFSMW.

### [pendente] Perfilar a thread do anel com simpleperf
- Usar `tools/perfil_android.sh` para achar onde vão o custo fixo (~18,5 ms) e o custo por draw,
  em vez de adivinhar "etapas". Observação: o `[tiron]` mostra cópias de vértices em ~0,3–0,5 ms;
  vértice/índice não é o gargalo.

### [pendente] Verificar o corte de área no S23 e num trecho com bloom/blur
- A comparação de imagem do corte de área foi só no PC. Repetir no S23 e num trecho com
  `320 (cubo y desenfoque)`, porque o tiling da Adreno difere.

### [pendente] Plano/patch: swap/restore do resolve de cor no FH2
Plano para a outra IA implementar. **Nada disto foi aplicado nesta sessão.**

#### Ponto de partida — por que hoje são 0 `intercambios`
O portão em `nfsmw_nativo_destinos.cpp:1826` exige **tudo**:
1. `nfsmw_nativo_intercambiar_color` (`:143`) — default **false** (off).
2. `ResolverSinCopia()` (`:4933`) — true (cvar default true, `:95`).
3. `borrar_color` — flag de clear da ordem (o FH2 satisfaz; ver instrumentação).
4. `fh2_sesgo == 0` — mas o resolve da cena HDR do FH2 usa **-2** e os blurs **-4** (`:1821-1822`),
   então cena/blur ficam **excluídos**; só resolves com `sesgo == 0` passam.
5. `entero_color` — o resolve tem de cobrir o destino inteiro (`:1813`).
E dentro de `IntercambiarConResuelta` (`:4958`), exige mesmo tamanho/formato (`:4959`).

Para a cena do FH2, quase nenhum se sustenta além do (3):
- **Tamanho**: o destino de cor é criado `1280x1280` (`alto = max(720, pitch)`, `:4576`) e a resolvida
  `1280x720` → `:4959` retorna false. Criar o destino com 720 de altura foi **testado e quebra** a cena
  (mancha amarela) e foi revertido (`:4571-4575`).
- **`entero_color`**: a cena é resolvida **em faixas** (0-256/256-512/512-720) para uma textura
  `1280x720` (`:1680-1687`) → nunca é "inteiro".
- **`fh2_sesgo`**: o resolve HDR precisa de `Fh2CopiarConEscala` com o clamp 7e3 (`:1848-1852`); um swap
  não escala → exposição errada.

Ou seja: **ligar o cvar sozinho não muda quase nada na cena**. A hipótese otimista do item
"[medido] Confirmação no S23" (o clear já vem, então o swap valeria) é **falsa para a cena** por causa
dos pontos acima; vale no máximo para resolves de cor de destino inteiro com `sesgo == 0`.

#### Mudanças propostas (cada uma reversível por cvar)
- **M1 (baixo risco, primeiro)** — default do cvar = true **só no FH2**:
  ```cpp
  #if defined(FH2_RECOMP)
  REXCVAR_DEFINE_BOOL(nfsmw_nativo_intercambiar_color, true, "NFSMW", "...");
  #else
  REXCVAR_DEFINE_BOOL(nfsmw_nativo_intercambiar_color, false, "NFSMW", "...");
  #endif
  ```
  NFSMW intacto. Esperado: poucos/nenhum intercâmbio na cena; pode pegar resolves de destino inteiro com
  `sesgo == 0` (LDR/post). Medir `intercambios` e `restauraciones`.
- **M2 (o ganho real, mais complexo)** — permitir swap em resolves **por faixa** e com destino maior que
  a textura:
  - Alocar as texturas resolvidas de cor no **tamanho cheio do destino** (`1280x1280`) quando o destino
    for o cenário, mantendo a visão guest em `1280x720` (`ancho_guest/alto_guest`, `:4594-4597`); assim o
    teste de tamanho de `:4959` casa.
  - Relaxar `entero_color` para "cobre toda a largura e a altura útil" (as faixas somam 0-720) e trocar as
    imagens quando a **última** faixa fecha o retângulo. Análogo ao `fh2_borrar_ahora`, que já junta clears
    de faixas contíguas (`:1893-1919`).
  - O clear no swap já está tratado: `Fh2AplicarBorrado(destino)` (`:4964`) e `OlvidarBorrado`/`RestaurarContenido`.
  - Risco: a "mancha amarela" histórica veio de mexer no tamanho da **imagem**; M2 mexe no tamanho da
    **resolvida**, não do RT, mas ainda expõe pitch/indexação — testar com A/B de imagem.
- **M3 (bloqueio duro / pesquisa)** — `fh2_sesgo != 0`: o resolve HDR (-2) e os blurs (-4) precisam da
  escala/clamp 7e3 que só o copy faz; um swap não escala. **Swap descartado** aqui. Alternativas a
  estudar depois: clamp na amostragem; RT host compatível com 7e3 (sem precisar escalar); scaling blit
  mais barato que o copy atual.

#### Invariantes de correção (checar no log/aparelho)
- `restauraciones` (no `C6`/`[tiron]`) tem de ficar **0** (como hoje). Se subir, o swap não vale.
- `intercambios` > 0 = disparou.
- A/B de imagem no mesmo ponto (`nfsmw_nativo_resolver_sin_copia_alternar_s`): sem mancha amarela e com a
  mesma exposição.
- `C6 reanudados por causa`: o `copia+borrado color` deve cair.

#### Medição antes de codar (recomendada)
Estender o diagnóstico já instrumentado para dividir `copia+borrado color` por: `sesgo == 0` vs `!= 0`,
`entero_color` vs faixa, e se é o destino do cenário. Assim sabemos o **tamanho do prêmio** de M1/M2 antes
de mexer no tamanho das imagens.

#### Ordem sugerida
1. Medição acima. 2. M1 + medir. 3. Se M1 não bastar, M2. 4. M3 só como pesquisa.

---

## 2026-10-12 — Sessão Claude Code: menu, idioma, FPS, drivers e vegetação

Tudo abaixo está na `main` (GitHub até `f6acd8b`), exceto o que estiver marcado como não commitado.
Aparelhos: S23U `RQCXB01R9LM` (Adreno 740) e moto g(9) play `0074545453` (Adreno 610, Android 16).

### [feito] Menu novo do app (`0fd9c8e`, `a340393`)
- **Arquivos**: `android/.../MainActivity.java`, `Menu.java` (novo), `SegmentedControl.java` (novo),
  `res/drawable/ic_menu_*.xml`, `themes.xml`.
- Barra lateral vertical (Gráficos, Tela, Desempenho, Som, Idioma, Jogo e saves, Sobre) e Jogar sempre visível.
  Gráficos começa pelo renderizador; abaixo só as opções dele.

### [feito, não verificado em partida] Idioma do jogo e do app (`a340393`)
- **Arquivos**: `AppLanguage.java` (novo), `GameOptions.java` (`GAME_LANGUAGES`), traduções nos Java.
- O jogo lê o idioma por `ExGetXConfigSetting(3, 9)` → cvar `user_language` (confirmado no código recompilado:
  `sub_82BFF958`) e a região por `XamGetLocaleEx` → `user_country`. O app passa os dois. 12 idiomas do disco
  (`media/stringtables`), 4 com dublagem (`media/audio/vo`: EN, BR, MX, FR).
- **Falta**: confirmar numa partida que o texto troca. A tentativa no S23 foi interrompida (o aparelho desconectou).

### [feito] Limite de FPS (`9c8fab2`)
- **Arquivos**: `GameOptions.java` (`vblankRate`), `MainActivity.java`, `sdk/src/kernel/xboxkrnl/xboxkrnl_video.cpp`
  (trava 24–480 Hz).
- O jogo apresenta a cada 2 vblanks no jogo (a cada 1 nas telas de início). O app passa
  `--video_mode_refresh_rate` = 2 × limite (Livre = 480 Hz). Padrão passou a 30.
- **Medido no PC** (`tools/teste_jogabilidade.sh`): a 120 Hz os carregamentos foram de 55 para 104 fps e
  apareceram quadros < 30 ms no jogo (nenhum a 60 Hz). Velocímetro igual após o mesmo tempo de acelerador →
  o relógio do jogo é o real (`KeQueryPerformanceFrequency`), a física não acelera.
- No PC a corrida não passa de ~29 fps por desempenho (anel do nativo 998 ms/s), não pela trava.
- **Não testado**: corrida acima de 30 fps num celular.

### [feito] Drivers de GPU (`f6acd8b`)
- **Arquivos**: `sdk/thirdparty/libadrenotools` (novo, BSD-2), `sdk/src/ui/vulkan/android_gpu_driver.cpp`,
  `vulkan_instance.cpp`, `dynlib.h`, `android/app/src/main/cpp/android_diagnostico.cpp`, `GpuDrivers.java`,
  `GpuProbeActivity.java` (novo, processo `:gpu_probe`), `Diagnostics.java`, `MainActivity.java`.
- Drivers baixados para teste (em `/sdcard/Download/fh2-drivers/` nos dois aparelhos):
  K11MCH1 `Turnip_v26.0.0_R8.zip`, Weab-chan `turnip_Oct-12-2025_bbdd688_8g2_ui_glitch.zip`,
  MrPurple666 `turnip_mrpurple_T30-toasted.adpkg.zip`, K11MCH1 `Qualcomm_840_adpkg.zip`.
- **Resultado do teste Vulkan** (requisitos do renderizador nativo):

  | Driver | S23U (Adreno 740) | moto g9 play (Adreno 610) |
  |---|---|---|
  | Turnip v26 R8 | passa, Vulkan 1.4.335 | falha: Vulkan 1.0, sem descriptor indexing |
  | Turnip Oct/2025 8g2 | passa, Vulkan 1.4.328 | falha: Vulkan 1.0 |
  | Turnip T30 (PurpleVK) | passa, Vulkan 1.4.359 | **passa, Vulkan 1.3.359** |
  | Qualcomm 840 | passa (carrega de fato) | não carrega: falta `libdmabufheap.so` |

- **Em partida**: S23 nativo + Turnip R8 (`fh2_20261012_140725.log`: `Vulkan device 'Turnip Adreno (TM) 740'`);
  S23 **emulado** + T30 (`fh2_20261012_141135.log`, do usuário: ~16,7 fps; emulado com o driver de fábrica
  ~12–15 fps); moto nativo + T30 (`fh2_20261012_140640.log`). Moto emulado + T30: **não testado**.
- **Armadilhas já encontradas (não repetir)**:
  - O APK guarda as `.so` dentro dele; `nativeLibraryDir` fica vazio e a adrenotools não acha `libhook_impl.so`
    (sintoma: o driver "carrega" mas não acha GPU). Solução: `GpuDrivers.hookFolder` copia só os hooks e o
    `libc++_shared.so` do APK. Não usar `useLegacyPackaging` (extrairia ~300 MB).
  - A adrenotools volta para o driver de fábrica **sem erro** quando o importado não carrega; a sonda confere
    em `/proc/self/maps` (com `realpath`: `/data/user/0` é atalho de `/data/data`).
  - Driver Vulkan 1.0: ler recursos com `vkGetPhysicalDeviceFeatures2KHR`.
- O teste só confere os requisitos do **nativo**; o R8 talvez rode no emulado do moto (não testado).

### [medido] Testes por adb: o que funciona e o que não
- Importar/escolher driver: `am start ... --es fh2.importar_driver <zip>` / `--es fh2.escolher_driver <nome>`
  (resultado no logcat, tag `FH2`, `Vulkan test of`). Fazer `am force-stop` antes: com o app aberto o
  `onNewIntent` **não** entrega os extras (o mesmo vale para `fh2.banco`). Nome vazio: `--es fh2.escolher_driver ''`
  dentro de aspas no `adb shell "..."`.
- O logcat corta linhas longas: o JSON do teste chega truncado depois da lista de extensões.
- `adb shell input tap/swipe` **não é confiável** com o usuário mexendo no aparelho (toques caem em outro lugar,
  ajustes mudam). Antes de qualquer teste no celular, olhar `dumpsys activity activities | grep topResumedActivity`.
- No PC, `SEGUNDOS=N tools/rodar_pc.sh` sem roteiro de botões **fica nos vídeos de abertura**: usar
  `tools/teste_jogabilidade.sh` (pula os vídeos, dirige de 60 a 85 s, capturas a cada 10 s em
  `app/out/linux/capturas`). Os testes no PC abrem a janela do jogo na tela do usuário.
- `pkill -f <padrão>` dentro de um comando que contém o padrão mata o próprio comando.

### [não commitado] Alpha to coverage pontilhado como na emulação (folhas, cercas, público)
- **Arquivos**: `tools/xenos/recomp/XenosRecomp/shader_common.h` (`SPEC_CONSTANT_A2C_PONTILHADO`, bit 21;
  `alphaTestValue(alpha, posição)`), `shader_recompiler.cpp` (passa `iPos.xy`), `app/src/nativo/nfsmw_nativo_dibujos.cpp`
  (cvar `fh2_nativo_a2c_pontilhado`, padrão true; offsets de `RB_COLORCONTROL >> 24` em `g_AlphaThreshold`).
- **Por quê**: o emulado com `gpu_sin_msaa` (a configuração do usuário: FXAA + sem MSAA) usa o ramo 1x de
  `spirv_translator_rb.cpp`: limiar por pixel `1 − offset/4` num bloco 2×2. O nativo cortava em 0,5 fixo.
- **Resultado no PC**: copas das árvores um pouco mais cheias; diferença pequena. Exige regerar a biblioteca.
- **Não foi para os celulares.**

### [feito no PC] Grama corrigida; poeira/fumaça das rodas passam a aparecer (pretas); k_DXT3A
- **Grama**: com o swizzle reescrito (item abaixo), os tufos ficam em pé, com lâminas e amarelados como no emulado
  (capturas aos 60 s, PC). **Exige a biblioteca nova** (ver o problema da noite clara antes de enviá-la aos celulares).
- **Partículas do carro** (poeira, fumaça): VS n0, 506, 620, 1887, 3044, 3600 (`v_003718`–`v_003723`, constantes
  `CarPosition`, `CarRightVector`, `BeginColor`...). Também usam elementos diretos: antes saíam invisíveis; com o
  swizzle certo aparecem, mas como borrões **pretos opacos** (mistura `07010701` = ONE / ONE_MINUS_SRC_ALPHA, cor 0).
  Confirmado pulando esses VS (`--fh2_nativo_diag_omitir_vs=0,506,620,1887,3044,3600`): os borrões somem.
  A cor delas vem de iluminação (`LightColorAmb`, `LightInfluences`); suspeita: o mesmo problema dos faróis (abaixo).
- **k_DXT3A** (formato 58; máscaras de alfa): agora suportado, cada bloco vira um bloco BC2 (alfa do jogo + cor
  branca) com o alfa em todos os canais. Não era a causa da poeira preta.
- Textura vazia do nativo para formato não suportado = (0,0,0,0) (com alfa 0, partícula seria descartada, não preta).

### [medido, aberto] Noite clara / faróis sem cone: booleana 228 e o mapa de sombra dos faróis
- `--fh2_nativo_diag_booleanos_apagar=16-127,144-255` (zera as booleanas que a biblioteca antiga não recebia) deixa a
  noite escura de novo. O jogo só liga uma booleana alta: **228** = booleana 100 dos PS,
  `bEnableDeferredLightContribution` (106 PS a usam). `--fh2_nativo_diag_luces=true` registra as ligadas.
- As constantes dos faróis chegam **iguais** às do emulado (`HeadLightParams1` c168 ≈ (8900, 40, −8000, 1),
  `HeadLightParams2` c169 = (0,2; 0,8; 60; 0); traço do emulado e `--fh2_nativo_diag_luces`).
- **Pista falsa (descartada)**: `clamp(log2(x), FLT_MIN, FLT_MAX)` parecia zerar logs negativos, mas neste projeto
  `FLT_MIN` é `asfloat(0xff7fffff)` = −FLT_MAX: o clamp está certo.
- **Causa provável**: o bloco dos faróis (ex. `p_002953`) faz `log2` dos canais de `ShadowMaskSamp` (`r9`, a máscara de
  sombra da tela). Canal 0 → −∞ → luz gigante em todo pixel. A máscara é feita por PS que leem no **registro 8** a
  profundidade 1024x1024 `k_24_8` em **`1D6C5000`** (PS n847, n1402, n3238; draws em `sup 04010140`), que o nativo
  **nunca escreve** (só resolve 1024x1024 em `1CA95000`, `1CE95000`, `1D295000`) e lê como textura vazia.
  Provavelmente é o mapa de sombra dos faróis. **Próximo passo**: descobrir quem escreve `1D6C5000` no emulado
  (traço de vários quadros, ou procurar resolves/memexport com destino nessa faixa) e fazer o nativo produzi-lo.
- Até lá, uma biblioteca nova nos celulares traria a noite clara (a antiga, de 05:46, não liga a booleana 228).
- **Atualização (mesmo dia, depois):**
  - `1D6C5000` **não é dos faróis**: é `CSMShadowMapLevel0Samp` (1º nível do mapa de sombras em cascata do sol;
    registro 8 de `p_003381`/PS n3238) e também a textura do registro 0 de partículas. O emulado não o reescreve:
    10 quadros traçados durante a direção (`--gpu_traco_quadro_s=70 --gpu_traco_quadros=10`) não têm nenhum RESOLVE
    para ele nem nenhum 1024x1024. A memória tem profundidades de 0 a 1.
  - **Feito**: `k_24_8` lido da memória vira `R32_SFLOAT` em R (24 bits de cima / 2^24−1), como o emulado
    (`kLoadShaderIndexDepthUnorm`). Efeito: **os borrões pretos da poeira sumiram** (viram partículas pequenas
    escuras); a noite continuou clara.
  - A máscara de sombra da tela é `1CE2D000` (registro 13, `ShadowMaskSamp`, 1280x720 8888), copiada 1×/quadro nos
    dois renderizadores. No emulado (`--readback_resolve=fast` + `tools/despejo_png.py`) os 4 bytes têm média
    0,94–0,99, nenhum zero. Trocá-la por cinza no nativo (`--fh2_nativo_diag_vaciar=1CE2D000
    --nfsmw_nativo_diag_vacias_grises=true`) muda toda a luz da cena: **é a peça central da noite clara**.
  - Sequência no emulado antes da cópia da máscara: 558 retângulo sem PS (limpeza da base 720), 559 quad
    `PS 58CE51…` na base 720 (ONE/ZERO), 560–561 volumes de stencil (prim 8 e prim 4 n 36), 562+ quads
    `PS 232440…` com mistura `00080008` (multiplicativa, DST_COLOR × ZERO), um por luz. No nativo o 559 é
    VS n2289 + PS n2916 (`p_004017`: média 2x2 de s1.x em R, G = 1, B/A não escritos) lendo `t1=1CE2D000`, e só
    **um** draw `00080008` no quadro traçado (VS n218 + PS n3005).
  - **Medição que NÃO funciona (não repetir)**: estatística dos bytes no `EscribirLecturas` com
    `nfsmw_nativo_leer_resueltas_texels=2000000` deu 100 % zeros até para a cena final `1DAC5000`: a leitura de volta
    não traz os dados dessas texturas. A conclusão "máscara toda zerada" ficou **sem prova**; o diagnóstico foi removido.
  - Comparação visual: a máscara do emulado (PNG do despejo) é branca com sombras azuladas do carro e das árvores; no
    mosaico do nativo (`nfsmw_nativo_diag_resueltas=true`, captura aos 70 s) quase todas as texturas copiadas saem pretas
    (coerente com máscara preta, mas não prova).
  - Sequência do nativo antes da cópia da máscara, igual à do emulado: 24 pontos (VS n1744), limpeza (retângulo
    VS n2310 PS n1137, modo 5), quad VS n2289 PS n2916 (`mascara 0000000F`, lê `t1=1CE2D000`), retângulo, volume de
    stencil (VS n1502, prim 4 n 36), quad de luz VS n218 PS n3005 (`mascara 0000000E`, mistura `00080008`, lê
    `t0=1DAC5000 t1=138FF000 t2=134D9000`).
  - **Testes negativos (não repetir)**: (1) o tradutor já trata as constantes 0/1 das exportações como o Xenia
    (`zeroMask`/`oneMask`); (2) o emulado também inicia as saídas de cor em 0 (`spirv_translator.cpp`, "Initialize the
    colors for safety"); (3) restringir a escrita do PS n2916 a R e G (`--fh2_nativo_diag_mascara_ps=2916:3`, cvar novo)
    não muda a noite.
  - **Próximas pistas**: o que o quad de luz PS n3005 lê (`t0=1DAC5000` é a cena 1280x720 em cor; um quad de luz
    diferida costuma precisar da **profundidade** da cena: conferir se no emulado esse endereço tem outra coisa no
    momento do draw); comparar as constantes desse PS nos dois; ver se a limpeza (PS n1137, modo 5) escreve 1 no nativo.

### [feito] Faróis: a máscara começava preta (alias da EDRAM), 2026-10-13
- **Causa**: a máscara dos faróis (cor 8_8_8_8, EDRAM base 720, pitch 1280, 1x) precisa começar em 1 nos quatro
  canais: o passe base (`p_004017`) só escreve onde passa o teste de Z (céu) e os quads dos faróis **multiplicam**
  nela. O jogo não a limpa: antes, desenha um retângulo só de profundidade (VS n2310, `modo 5`) num buffer de
  profundidade 4x MSAA de pitch 640 **na mesma base 720**, com Z sempre 1.0 e stencil 0xFF (replace). Na EDRAM são os
  mesmos blocos e as mesmas palavras de 32 bits, então todas viram `FFFFFFFF`. O dump do emulado
  (`traco_despejo/0518_1CE2D000...bin`) tem 86 % das palavras em `FFFFFFFF`.
- No nativo, cor e profundidade são imagens separadas: a máscara ficava preta (foto da origem da cópia, com
  `--nfsmw_nativo_diag_resueltas=true --fh2_nativo_foto_copia=1CE2D000 --fh2_nativo_foto_completa=true`), e os
  shaders do mundo fazem `−log2(máscara)` → luz infinita → noite como dia.
- **Prova**: `--fh2_nativo_diag_vaciar=1CE2D000 --fh2_nativo_diag_vacias_blancas=true` (máscara lida como 1) deixou a
  noite escura.
- **Correção**: `Fh2RellenoEdramMascara` (`nfsmw_nativo_destinos.cpp`). Ao ver esse retângulo (modo 5, 4x MSAA, Z
  sempre com escrita, stencil replace com ref e máscara de escrita 0xFF), limpa com 1 a imagem de cor 8_8_8_8 da mesma
  base com o dobro do pitch. `fh2_nativo_luces_diferidas` voltou a `true` por padrão: noite escura e cone do farol
  no chão, como no emulado (capturas de 60 e 80 s).
- **Pistas falsas desta investigação (não repetir)**: a segunda cópia para `1CE2D000` a partir da base 000 em ~20 %
  dos quadros (pulá-la não muda nada); o cache de texturas entre quadros; o swizzle `60A`→`688` (é a identidade);
  tirar o teste de Z do passe base.

### [medido, resolvido acima] Faróis: rodada de testes de 2026-10-13
- **Já verificado igual ao emulado (não repetir)**: a profundidade lida pelo projetor (`1DAC5000` é a cópia da
  profundidade, `control 4`, empacotada em 8888: o nativo usa `fh2_profundidad_rgba8`, "empaquetada si", e os bytes do
  emulado batem com esse empacotamento); as constantes do projetor (PS n3005 = `p_003825`: `undoProjection` c0–c1,
  `lightTransform` c2–c4, `lightParams1/2` c5–c6, cor c7); a geometria do quad (lista de retângulos, 4º canto certo,
  `--fh2_nativo_diag_ps=3005`); as constantes dos faróis no terreno (c168, c169).
- O projetor (`p_003825`) lê a profundidade (`.x`), reconstrói a posição, usa o feixe 2D (`samplerBeam`) e a queda 1D
  (`samplerFalloff`) e escreve `exp2(−luz)` em G/B/A da máscara (mistura multiplicativa). Os 106 PS do mundo somam
  `−log2(máscara) · N·L · 0,2`, sem textura. O emulado tem **dois** quads de projetor por quadro (um por farol), o
  nativo **um**.
- **Testes negativos (não repetir)**, todos com `--fh2_nativo_luces_diferidas=true`, a noite continuou clara:
  queda ou feixe trocados por cinza (`fh2_nativo_diag_vaciar` com endereços de outra rodada: talvez nem tenham
  pegado); base da máscara (`p_004017`) escrevendo B/A = 1 (com e sem o projetor); máscara inteira = 1 e projetor
  pulado (`--nfsmw_nativo_diag_omitir_ps=3005`); sem cópias adiadas (`--nfsmw_nativo_frontal_perezoso=false
  --nfsmw_nativo_resolver_sin_copia=false`). Edições de shader feitas com `tools/xenos/recompilar_um.sh` (o
  `p_002953` tem o gêmeo `p_002960`, mesmo microcódigo: editar os dois) e revertidas.
- **Contradição em aberto**: trocar `1CE2D000` por cinza via `fh2_nativo_diag_vaciar` muda a luz, mas fazer o pass
  da máscara escrever 1 não muda. Ou os materiais visíveis não leem a máscara que esse pass produz, ou a luz extra vem
  de outro termo. A visualização do que o `p_002953` lê não apareceu: ele não desenha as superfícies do trajeto.
- **Observação do usuário**: no celular o cone do farol às vezes aparece **em outro lugar**, sem relação com a noite
  clara: sugere posição reconstruída errada (projetor) ou máscara de outro quadro.
- **Próximo passo sugerido**: achar os PS das rochas/estrada (bisseção com `nfsmw_nativo_diag_omitir_ps`) e
  visualizar neles o que leem da máscara; verificar por que o nativo desenha um só quad de projetor.

### [feito] Toast de conquista pequeno e centralizado (2026-10-12, corrigido em 2026-10-13)
- **Arquivo**: `sdk/src/ui/overlay/achievement_toast.cpp`.
- **Por que ficava gigante**: no Android o ImGui desenha numa tela lógica (pixels físicos ÷ DPI), com só ~300 linhas
  de altura. O toast fixo de 320×70 ocupava um quarto da tela.
- **Primeira tentativa (`70d6fbf`) não funcionava**: posicionava o canto de **cima** da janela a 3 % do fim da tela,
  então a janela descia para fora dela. Também centralizava pela largura máxima, não pela real, e não mudava o
  tamanho.
- **Correção**: tamanho proporcional à tela (~12 % da altura; nunca maior que no PC, nunca menor que metade, com fonte,
  ícone e margens na mesma escala), ancorado pelo meio da borda de baixo (`pivot (0.5, 1)`) a 3 % do fim. O nome
  quebra linha dentro da largura, com a pontuação junto.
- **Teste**: `--achievement_toast_test=true` mostra um toast de exemplo o tempo todo (a janela é criada já no início
  nesse modo). No PC: pequeno, centralizado embaixo, com quebra de linha. No Android: `am start` com
  `--esa fh2.args --achievement_toast_test=true`.

### [corrigido] Pool de texturas: devolver blocos vazios ao sistema (2026-10-13)
- **Branch**: `perf/ram-gpu-optimizations`. Arquivos `app/src/nativo/nfsmw_nativo_texturas_pool.h/.cpp`.
- **Ideia (`22bbece`)**: o pool reservava blocos de 32 MB e nunca os devolvia. Cvars `nfsmw_nativo_texturas_pool_encoger`
  (padrão true) e `nfsmw_nativo_texturas_pool_encoger_espera` (300 quadros).
- **Bug da primeira versão**: apagava o bloco do vetor. Cada textura guarda o bloco pelo índice (`bloque = slab << 24
  | início`), então os blocos seguintes mudavam de índice e suas texturas liberavam unidades do bloco errado. No PC
  isso aconteceu (bloco 1 devolvido com blocos depois dele). Também reduzia o teto a cada devolução e devolvia os
  blocos pré-aquecidos, que logo eram recriados (memset de 32 MB cada).
- **Correção**: o bloco devolvido fica no vetor sem memória (pulado por `Reservar`, `Estado` e pelo cálculo de folga) e
  a vaga é reaproveitada quando o pool cresce; o teto não muda; nunca encolhe abaixo dos blocos pré-aquecidos.

### [medido, aberto] Colisão restante (~10 %): comum ao nosso SDK e ao ShiftGlue (2026-10-13)
- Depois das contas vetoriais, o usuário avalia a colisão em ~90 %: objetos caem certo, mas às vezes carros sobem uns
  nos outros e um poste atingido por um oponente ricocheteou e voou rápido demais.
- **No ShiftGlue do PC acontece igual** (mesmo save, limite de 30 fps, ~26 fps): não é algo que o SDK deles corrige.
  No celular o ShiftGlue rodava a ~13 fps e o usuário não tinha notado.
- O código gerado das 85.237 funções foi comparado com o do ShiftGlue (normalizando rótulos, entrada pelo meio,
  pilha, barreiras e chamadas de cauda): a matemática é a mesma. Sobram a forma do `stwcx` (CAS) e ~150 chamadas de
  segurança depois de funções que não retornam.
- Diferença que resta: com o mesmo save, a guarda de `sub_82ECEB88` ("formas a distancia cero") dispara 4 vezes ao
  carregar no nosso e nenhuma no ShiftGlue.
- **Testes negativos (não repetir)**: desligar essa guarda (a resposta "em contato") não mudou nada para o usuário e
  o jogo não travou sem ela (o travamento de 11/10 era das contas vetoriais); os ganchos de `fh2_desempenho.cpp` só
  mexem no anel e nas faixas da cena; o relógio do nosso SDK espera o outro thread e devolve um tempo atual.
- **Próxima hipótese**: o passo de tempo da física. No Xbox o FH2 roda travado a 30 fps; aqui o ritmo oscila nos
  dois SDKs. Achar como o jogo mede o tempo do quadro e testar com 1/30 s fixo.

### [feito] Queda a ~4 fps no festival: tabela de buscas vigiladas cheia (2026-10-13)
- **Sintoma**: no S23 Ultra, ao chegar ao festival, de ~25–30 para ~4,5 fps; GPU em ~30 %, a thread do anel do nativo
  em 80–84 %. Reproduzido no PC dirigindo até o festival (quadros de 75–150 ms).
- **Medido** (`[tiron] envios por linea`, contagem nova dos envios de trabalho por linha de `nfsmw_nativo_destinos.cpp`):
  16–17 envios por quadro em `EnviarYEsperar` (buffer de subida cheio). O buffer enchia com **buscas** (dados de
  vértice lidos pelos shaders): ~100 GB a cada 10 s, motivo **tabla llena**.
- **Causa**: a tabela `fh2_vigilados_` (16.384 faixas ≥ 4 KB reaproveitadas no quadro enquanto o jogo não as escreve)
  só crescia. No festival ela enche em ~20 s; cheia, toda faixa fora dela era copiada **a cada desenho** em vez de uma
  vez por quadro.
- **Correção**: quando enche, recomeça (`fh2_vigilados_reinicios_`). No PC, mesma viagem: a tabela recomeçou 5 vezes,
  "tabla llena" 0, buscas estáveis em ~1,7 GB/10 s e os quadros no ritmo normal.

### [feito] Colisões atravessadas: contas vetoriais diferentes das do Xenon (2026-10-14)
- **Sintoma**: o carro atravessava carros, paredes e postes (nativo e emulado, PC e celular). No FH2 sobre o SDK do
  Pinyon Shift (branch `comparacao/shiftglue`) a colisão funciona com o mesmo jogo, manifesto e correções.
- **Pista medida**: a guarda de `sub_82ECEB88` ("formas a distancia cero tratadas como en contacto",
  `fh2_correcoes.cpp`) disparava no nosso e **nunca** no ShiftGlue, na mesma rota do PC: a função de distância entre
  formas de colisão recebia pontos idênticos. A guarda de `sub_82EC93F8` ("cara N de una malla de M") dispara nos dois:
  não explica a diferença.
- **Causa**: o gerador e os auxiliares de PowerPC do ShiftGlue fazem como o Xenon contas que o nosso fazia como x86:
  `vmsum3fp128`/`vmsum4fp128` (produto escalar em double, na ordem do hardware, QNaN no estouro; o comentário deles diz
  que o `simde_mm_dp_ps` "muda comparações no código de colisão"), `vmaddfp`/`vnmsubfp` fundidos (FMA),
  `vmaxfp`/`vminfp` (NaN, zeros com sinal, denormais), `vrsqrtefp`/`frsqrte`/`vexptefp` (negativos e faixa),
  `vcmpbfp` (NaN e CR6), `vsr`/`vsl` (byte da contagem e transporte entre metades).
- **Correção**: portados do ShiftGlue (BSD-3) para `sdk/include/rex/ppc/intrinsics.h`, `sdk/include/rex/ppc/context.h`
  (`simde/x86/fma.h`), `sdk/src/codegen/builders/vector.cpp` e `floating_point.cpp`; código regenerado (apagar
  `generated/*/codegen.stamp`, senão o codegen diz "up to date" e não reescreve nada).
- **Resultado no PC**: a guarda de distância zero não dispara mais (0 no nativo e no emulado); aos 80 s da rota o carro
  para no poste e na grade, como no ShiftGlue (antes passava a 36 km/h).

### [medido] Avaliação: o FH2 no SDK do Pinyon Shift (ShiftGlue), 2026-10-13
- **O que é**: [Pinyon Shift](https://github.com/arcanite24/pinyon-shift) recompila o FH1 (mesma engine) com o
  [ShiftGlue](https://github.com/arcanite24/shiftglue-sdk), fork BSD-3 do ReXGlue 0.10.0 (a mesma versão do nosso).
  O "renderizador nativo" deles é o `Fh1NativeExecutor` (`src/graphics/vulkan/fh1_native_executor.cpp`, ~3.600
  linhas) dentro do backend Vulkan do Xenia: substitui só a EDRAM (superfícies por base/pitch/formato/MSAA, mapa de
  posse das 2048 baldosas com transferências entre aliases, resolves direto nas texturas, clears dobrados). Os shaders
  continuam traduzidos em tempo de execução, como no nosso emulado: **não precisa de `fh2_shaders.nfsp`**. Não há
  nenhum endereço ou title ID do FH1 no executor (o title ID só carrega um pacote opcional de shaders do FH1).
- **Protótipo** (fora do repositório, `referencias/fh2-shiftglue`): o nosso `fh2_manifest.toml`, os overrides, as
  correções do FH2 (`fh2_correcoes.cpp`), a entrada e as capturas de teste, um `main` mínimo, `GPU_PLUGINS fh1`.
  O codegen deles aceitou o manifesto sem mudança (8 min, 719 arquivos); compila com `-march=x86-64-v2`; roda com
  `--gpu_plugin=fh1`.
- **Resultado no PC (RADV Renoir)**: **o FH2 renderiza certo de primeira** na rota de teste noturna: noite escura,
  faróis no chão, HUD, folhagem; com e sem `fh1_untile_predicated_tiling`; `skips={}` em todos os relatórios (nenhum
  desenho desconhecido). O problema da máscara dos faróis (alias da EDRAM) não existe lá.
- **Desempenho no PC**: inconclusivo. O contador de quadros do executor ficou em ~30 fps nos menus e ~27 na direção
  mesmo com `pinyon_shift_fh1_render_fps_limit=120` e vblank a 240 Hz (o nosso nativo, na mesma rota: 46–57 fps, sem
  MSAA e com outro custo). As transferências de EDRAM entre a profundidade 4x e 1x da base 0 pesam no FH2
  (`0/16/4x/d1->0/16/1x/d1`, milhões de tile-passes em 600 quadros).
- **Decisivo e ainda não feito**: o APK do protótipo no S23U (o SDK deles já tem Android, Turnip e adrenotools).

### [feito] Qualidade das sombras funcionando no FH2 (2026-10-13)
- **Por que não funcionava**: a escala e o desligar reconheciam o mapa de sombras pelo pitch 1600, que é o do NFSMW.
  O FH2 nunca usa 1600 (o usuário testou no S23U: slider sem efeito e sombras no 0).
- **As sombras do FH2** (quadro traçado de dia no S23U, `--nfsmw_nativo_diag_fotograma_s`): 4 cascatas do sol por
  quadro, cada uma desenhada num alvo de profundidade de pitch **1040** (EDRAM base 720, `sup 10000410`, ~560 desenhos,
  `LEQUAL`) e resolvida como 1024×1024 em `1CA95000`, `1CE95000`, `1D295000` e `1D6C5000`. Só existem de dia: os
  testes do PC (à noite) nunca as tinham.
- **Escala**: o caminho do NFSMW (blit de profundidade) não serve — nem o driver do S23U nem o do PC escalam
  profundidade. As cascatas agora passam pelo mesmo caminho da escala de render do nativo (`Fh2Escalar`): alvo
  menor, enquadramento escalado e cópia 1:1 para textura do mesmo tamanho.
- **0 = desligadas**: os desenhos no alvo de 1040 são descartados e, antes de cada cópia, o alvo é limpo a 1.0 (longe),
  então nada projeta sombra.
- **Teste no PC com o save de dia do S23U** (`app/out/save_s23_dia`, `USUARIO=` no `rodar_pc.sh`): 100 % com sombra do
  carro, da tenda e da grade; 25 % as mesmas, mais suaves; 0 sem nenhuma sombra.

### [feito] Tela de conquistas no app (2026-10-13)
- **De onde vem a lista**: das conquistas no XDBF do executável, que só o jogo lê. A cada início o SDK grava
  `user/achievements/catalog_4D530AA4.json` (nome, descrição, descrição bloqueada, pontos, imagem, flags) e os ícones
  em `user/achievements/icons/<imagem>.png` (`ExportAchievementCatalog`, `sdk/src/system/kernel_state.cpp`). Os
  desbloqueios são o save do próprio SDK ao lado, `4D530AA4.toml`. No PC: 46 conquistas, 1000 G, 46 ícones PNG 64×64.
- **App**: página "Conquistas" abaixo de Som (`Achievements.java`, `MainActivity.achievementsPage`). Resumo com
  desbloqueadas/total, pontos e barra magenta; desbloqueadas primeiro (mais recente no topo) com ícone colorido, anel,
  data e pontos em magenta; bloqueadas em cinza, ícone sem cor e cadeado. Sem a flag 0x8 do XDBF a conquista é
  secreta: nome e ícone escondidos até desbloquear. Sem catálogo (jogo nunca aberto) a página explica isso. Recarrega
  ao voltar do jogo.

### [feito, não testado] Opções de gráfico no menu em vez de padrões escondidos (2026-10-13)
- Os valores iniciais que a branch `perf/ram-gpu-optimizations` tinha baixado voltaram aos de antes (escala 100 %,
  anisotrópico 4x, distância das sombras "longe", reflexos "altos"); quem tem celular fraco baixa no menu.
- **Qualidade das sombras** (nativo): slider de 0 a 100 % (passos de 5) → `nfsmw_nativo_sombras_escala`, padrão 100.
  Cada valor vale como escolhido (5 % = 80×80; o lado mínimo da imagem é 64). **0 desliga as sombras**: os desenhos no mapa de sombras (pitch 1600) são
  descartados em `DestinosVulkan::Dibujar` e o mapa fica no valor do borrado, então nada projeta sombra.
- **Compilar shaders em segundo plano** (emulado): `async_shader_compilation`, padrão desligado (como antes).
- **Cópia adiada da cena** (nativo): `nfsmw_nativo_compuesta_perezosa`, padrão desligado (como antes; o padrão do C++
  também voltou a false).
- O usuário pediu para não testar no jogo: só compilado (PC e Java do Android).

### [removido] Troca de imagens por faixas na resolução da cena (`2ed1d29`, 2026-10-13)
- Nunca era executada: exigia viés de expoente 0 (a cena resolve com −2) e um alvo de 1280 px com escala ativa (com
  escala o alvo é menor). Código apagado; não há ganho a medir.

### [feito] Biblioteca nova nos celulares com os faróis diferidos desligados (2026-10-12, noite)
- Cvar `fh2_nativo_luces_diferidas` (padrão **false**): zera a booleana 228 em todo draw. Com ela desligada, a
  biblioteca nova (grama certa, poeira sem borrões) dá a noite escura como a de 05:46, sem o cone dos faróis no chão.
  Conferido no PC (capturas aos 60 e 70 s). Ligar (`--fh2_nativo_luces_diferidas=true`) só para trabalhar na noite clara.
- Biblioteca enviada: `app/out/shaders_fh2/fh2_shaders.nfsp` de 19:14 (75 497 272 bytes), com backup da anterior em
  `jogos/ForzaHorizon2/fh2_shaders.nfsp.anterior`. moto e S23: feito (biblioteca + APK `a415b91`). No S23 o jogo está também como ISO em `jogos/`: sem biblioteca ao lado dela, vale a de `jogos/ForzaHorizon2`.

### [feito] Causa da grama achatada/marrom: swizzle dos elementos por instância
- **Achado** (diagnóstico `[fh2 nativo] elemento VS n.. #..`, uma linha por elemento e VS): nos fetches lidos direto
  (`fh2BuscaElemento`: grama, e provavelmente partículas e billboards), o shader traduzido aplicava o swizzle do
  **contêiner**, mas o D3D reescreve o `dst_swiz` (dword 1, bits 0–11) conforme a declaração do vértice.
- Grama = VS n3526 = `v_003789` (`python3 tools/xenos/indice_biblioteca.py 3526`). Elementos 0–4 iguais; diferentes:
  #5 (4 × half: largura/altura do tufo e fade) contêiner `.zwxy` × D3D `wzyx`; #6 (cor 8888) `.xywz` × `zywx`;
  #7 (cor 8888) `.xyz` × `zyx_` (vermelho↔azul). Explica tufo achatado e cor marrom.
- **Correção**: o app grava o swizzle reescrito em `FH2_COMPARTIDA_SWIZZLES` (bytes 1280–1407; `FH2_COMPARTIDA_BYTES`
  1408, `kPalabrasCompartidas` 352) e o tradutor emite `fh2SwizzleElemento(fh2BuscaElemento(...), ordinal)`.
  Exige biblioteca nova.

### [medido, aberto] Grama: tufos achatados no nativo
- Capturas aos 60 s (PC): no emulado os tufos ficam em pé com lâminas; no nativo parecem manchas marrons deitadas.
- No VS da grama (`v_003789`) o quad sai do índice do vértice (instância = índice/4, canto = `frac(índice/4)·4`,
  correto); o eixo "para cima" vem do elemento 1 (`fh2BuscaElemento(1, 1, instância)`). Hipótese: formato desse
  elemento decodificado errado. **Não testado.**

### [medido, aberto] Texturas sem formato no nativo (viram textura vazia)
- Log `[fh2 nativo] textura sin formato` (diagnóstico novo, não commitado, em `PrepararTextura`):
  - `k_24_8` 1024x1024 em `1D6C5000`, tiled: registro 8 nos PS n847 e n1402; registro 0 nos PS n2230, n2955,
    n3034, n3238, n3654, n3665 (mundo/terreno). O nativo só resolve profundidade 1024x1024 em `1CA95000`,
    `1CE95000` e `1D295000`; um quadro traçado do emulado aos 72 s não tem RESOLVE para `1D6C5000`.
  - `k_DXT3A` em `1CE6C000` 1x1 (PS n1311, n183) e 128x128 em `15BF7000`/`15CB8000` (PS n3220).
- Quem escreve `1D6C5000` e o papel dela: **não descoberto**.

### [medido] A noite parecendo dia no nativo do PC vem da biblioteca de shaders nova
- Mesmo executável (main + diagnósticos), mesmo trajeto: com a biblioteca do S23 de **05:46** a noite fica escura;
  com a biblioteca regenerada hoje (tradutor da `main`, que inclui `394ef9c`, as 256 constantes booleanas) a cena
  noturna fica clara como dia. O pontilhado desligado (`fh2_nativo_a2c_pontilhado=false`) não muda isso.
- **Os celulares não foram afetados**: usam a biblioteca de 05:46; nenhuma biblioteca nova foi enviada.
- **Não regerar nem enviar a biblioteca aos celulares** até achar a causa.
- A biblioteca de 05:46 já tinha o `r0` = índice do vértice (`f4b88fb`, a grama aparece) e é anterior a `394ef9c`
  (12:17). Entre ela e a nova entraram `394ef9c` (256 booleanas e texturas 1D no tradutor) e o pontilhado.
  **Suspeito principal: `394ef9c`** — se as booleanas chegam com a ordem/bit errados, os shaders de iluminação
  pegam o ramo errado (ex.: o de dia).
- **Isolamento ainda não feito**: a tentativa de gerar a biblioteca sem o pontilhado falhou porque o `git diff`
  saiu vazio (ver o item seguinte). Próximo passo: gerar a biblioteca com o tradutor de `0cf0f8c` (sem o pontilhado)
  e, se a noite continuar clara, com o de `0cf0f8c` menos `394ef9c`.

### [feito] Jogo lido direto da ISO, sem extrair
- **Arquivos**: `sdk/src/system/runtime.cpp` (`SetupVfs`: `game_data_root` que é arquivo → `DiscImageDevice`),
  `sdk/src/ui/rex_app.cpp` (aceita arquivo; a checagem do `default.xex` no disco só vale para pasta),
  `app/src/nativo/nfsmw_nativo_sistema.cpp` (`fh2_shaders.nfsp` ao lado da ISO ou em `fh2_nativo_biblioteca_dir`),
  `GameIso.java` (novo: caminho real a partir do seletor, validação do `MICROSOFT*XBOX*MEDIA`), `GameOptions.java`
  (`game_iso`), `GameActivity.java`, `MainActivity.java` ("Usar a ISO do jogo" / "Parar de usar a ISO").
- Substitui a versão anterior (outra IA), que rodava `python3 tools/fase1_extraer.py` **no Android** (não existe Python
  no celular nem o script no APK) e nem punha o botão na tela.
- **Testado no PC** com `Forza Horizon 2 (USA) (En,...).iso` (8,7 GB): o jogo monta a ISO, carrega `default.xex` e as
  duas DLLs pelo VFS, passa pelos menus e dirige (~19–23 fps no emulado, igual à pasta).
- **Não testado no celular** ainda. Limite conhecido: a ISO tem de ser escolhida por um caminho real (armazenamento
  interno ou cartão); por "Recentes" o seletor não dá o caminho. A edição (SHA do `default.xex`) não é conferida com ISO.
- Rodar no PC pela ISO: chamar `app/out/linux/fh2` direto com `--game_data_root=<arquivo.iso>` (o `rodar_pc.sh` já passa
  `--game_data_root` e o parser recusa a mesma opção duas vezes).

### [atenção] Outra sessão trabalhando na mesma pasta (2026-10-12, ~16:30)
- Criou a branch `iso-support` (não enviada ao GitHub) e fez `a6adc70` ("Add ISO support foundation…") e `3f9c86f`.
- O `a6adc70` levou junto arquivos que **não devem ir para o git**: o save do usuário
  (`B13EBABEBABEBABE/.../ForzaProfile`, `PlayerDatabase`, `marketplace/tmpfile`, ~23 MB) e o cache de shaders
  (`cache/shaders/shareable/4D530AA4.*`, gerado dos dados do jogo); além do gitlink `.claude/worktrees/visuais-colisao`,
  do pontilhado e dos diagnósticos do nativo ainda não commitados desta sessão e deste arquivo.
- A `main` não foi afetada (`f6acd8b`, igual ao GitHub). **Não enviar `iso-support` como está.**
- Duas sessões na mesma pasta trocam a branch e os arquivos uma da outra: usar um worktree por sessão.

---

## Convenções deste arquivo
- Datas no formato AAAA-MM-DD.
- Toda alteração de código lista os arquivos tocados; toda medição lista o log de origem.
- Itens que o outro agente já tinha feito antes desta sessão entram como referência apenas se
  forem relevantes para o que vem depois.
