# DawnOfTermina

Recompilação estática nativa (static recompilation / port HLE) de **The Legend of Zelda: Majora's Mask 3D** (Nintendo 3DS) para **C++20**, com renderização via **Vulkan**.

> **Este repositório não contém nenhum código, binário ou asset da Nintendo.** Você precisa de uma cópia legítima do jogo (`.cia`) que você mesmo tenha obtido. Veja o [Aviso legal](#aviso-legal).

## Visão geral

O código ARM11 original (`code.bin` do ExeFS) é traduzido automaticamente para C++ por um recompilador estático (`tools/recomp`, executado durante o build a partir da sua cópia do jogo — o código gerado nunca é distribuído), e subsistemas selecionados são analisados no Hopper Disassembler e transcritos à mão para C++ legível. Os serviços do Horizon OS (`fs`, `hid`, `gsp`, `dsp`, `cfg`, `ptm`, syscalls `svc`) são reimplementados em alto nível (HLE), e as listas de comando da GPU PICA200 são traduzidas para Vulkan. Os assets (RomFS) são lidos em tempo de execução a partir de uma extração local feita pelo usuário.

## Requisitos

| Dependência | Versão mínima | Uso |
|---|---|---|
| Compilador C++20 | GCC 12 / Clang 16 | build |
| CMake | 3.20 | build |
| Ninja ou Make | — | build |
| SDL3 | 3.2 | janela, input, (áudio) |
| Vulkan SDK (headers, loader) | 1.3 | renderização |
| glslang | 14+ | GLSL → SPIR-V em runtime |
| Python | 3.10 | scripts em `tools/` |

## Preparando os dados do jogo

1. Coloque sua cópia do jogo em qualquer pasta fora do controle de versão (ex.: `ISO/` ou `input/`, ambas ignoradas pelo Git).
2. Extraia ExeFS, `exheader.bin`, `code.bin` (descomprimido), `code.elf` (para o Hopper) e RomFS para `game_data/`:
   ```sh
   python3 tools/extract_cia.py "ISO/<seu jogo>.cia" game_data/
   ```

O extrator é Python puro (sem dependências externas) e exige um `.cia` **descriptografado**; ele avisa e aborta se o conteúdo estiver criptografado.

## Compilando

Com `game_data/` presente, o build também recompila o `code.bin` (≈2 min em 6 núcleos; requer ~4 GB de RAM livres).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
(cd build && ctest --output-on-failure)
./build/dawnoftermina game_data
```

## Arquitetura

```
src/
  core/         memória virtual do 3DS (FCRAM, VRAM, DSP RAM, TLS), tipos base, utilitários
  hle/          serviços do Horizon OS (srv, fs, hid, gsp, dsp, cfg, ptm) e syscalls svc
  gfx/vulkan/   tradutor PICA200 -> Vulkan (registradores, shaders, TEV, formatos de textura)
  game/         código do jogo transcrito do ARM11, organizado por subsistema e endereço original
tests/          self-test do runtime (ctest)
src/recomp/     runtime do código recompilado (contexto da CPU, flags, VFP, despacho, syscalls)
tools/recomp/   recompilador estático ARM/Thumb/VFP -> C++
tools/          extração do .cia, símbolos
```

Cada função transcrita mantém o endereço virtual original em comentário (`// Original VA: 0x00104A20`) para rastreabilidade 1:1.

## Status

| Etapa | Descrição | Status |
|---|---|---|
| 1 | Bootstrap do repositório, licença, documentação | ✅ |
| 2 | Pipeline de extração do `.cia` e preparação do binário para o Hopper | ✅ |
| 3 | Runtime, memória e HLE do Horizon OS | ✅ |
| 4 | Backend Vulkan e tradutor PICA200 | 🟡 draws em lote, vertex shaders na GPU e iluminação por fragmento; cenas com personagens renderizadas; faltam fog, sombras e cache de pipelines (engasgos na 1ª vez de cada cena) |
| 5 | Entry point (`_start` / `nnMain`) | ✅ boot do crt0 transcrito até a fronteira atual |
| 6 | Recompilador estático automático ARM11 → C++ | ✅ boot do SDK roda recompilado até a criação de threads |
| 7 | Threads do guest e serviços do sistema até o primeiro frame | ✅ título, criação de arquivo (teclado HLE) e cutscene inicial da Lost Woods a ~60 VBlanks/s |
| 8+ | Subsistemas do jogo (cenas, loop, áudio, colisão, atores, HUD) | ⏳ |

## Licença

O código-fonte original deste projeto é distribuído sob a **GNU General Public License v3.0 ou posterior** — veja [`LICENSE`](LICENSE).

## Aviso legal

DawnOfTermina é um projeto independente, sem afiliação com a Nintendo. *The Legend of Zelda* e *Majora's Mask* são marcas registradas da Nintendo. Este repositório não distribui ROMs, binários, assets ou código proprietário; o projeto funciona exclusivamente no modelo *bring-your-own-ROM*, exigindo uma cópia legalmente adquirida do jogo. Não use este projeto para pirataria.
