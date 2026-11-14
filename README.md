# DawnOfTermina

Recompilação estática nativa (static recompilation / port HLE) de **The Legend of Zelda: Majora's Mask 3D** (Nintendo 3DS) para **C++20**, com renderização via **Vulkan**.

> **Este repositório não contém nenhum código, binário ou asset da Nintendo.** Você precisa de uma cópia legítima do jogo (`.cia`) que você mesmo tenha obtido. Veja o [Aviso legal](#aviso-legal).

## Visão geral

O código ARM11 original (`code.bin` do ExeFS) é analisado no Hopper Disassembler e transcrito, função por função, para C++ legível. Os serviços do Horizon OS (`fs`, `hid`, `gsp`, `dsp`, `cfg`, `ptm`, syscalls `svc`) são reimplementados em alto nível (HLE), e as listas de comando da GPU PICA200 são traduzidas para Vulkan. Os assets (RomFS) são lidos em tempo de execução a partir de uma extração local feita pelo usuário.

## Requisitos

| Dependência | Versão mínima | Uso |
|---|---|---|
| Compilador C++20 | GCC 12 / Clang 16 | build |
| CMake | 3.20 | build |
| Ninja ou Make | — | build |
| SDL3 | 3.2 | janela, input, (áudio) |
| Vulkan SDK (headers, loader, `glslc`) | 1.3 | renderização (Etapa 4) |
| Python | 3.10 | scripts em `tools/` |

## Preparando os dados do jogo

1. Coloque sua cópia do jogo em qualquer pasta fora do controle de versão (ex.: `ISO/` ou `input/`, ambas ignoradas pelo Git).
2. Extraia ExeFS, `exheader.bin`, `code.bin` (descomprimido), `code.elf` (para o Hopper) e RomFS para `game_data/`:
   ```sh
   python3 tools/extract_cia.py "ISO/<seu jogo>.cia" game_data/
   ```

O extrator é Python puro (sem dependências externas) e exige um `.cia` **descriptografado**; ele avisa e aborta se o conteúdo estiver criptografado.

## Compilando

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
tools/          extração do .cia, descompressão do code.bin, geração de stubs/símbolos
```

Cada função transcrita mantém o endereço virtual original em comentário (`// Original VA: 0x00104A20`) para rastreabilidade 1:1.

## Status

| Etapa | Descrição | Status |
|---|---|---|
| 1 | Bootstrap do repositório, licença, documentação | ✅ |
| 2 | Pipeline de extração do `.cia` e preparação do binário para o Hopper | ✅ |
| 3 | Runtime, memória e HLE do Horizon OS | ✅ |
| 4 | Backend Vulkan e tradutor PICA200 | ⏳ |
| 5 | Entry point (`_start` / `nnMain`) | ⏳ |
| 6+ | Subsistemas do jogo (cenas, loop, áudio, colisão, atores, HUD) | ⏳ |

## Licença

O código-fonte original deste projeto é distribuído sob a **GNU General Public License v3.0 ou posterior** — veja [`LICENSE`](LICENSE).

## Aviso legal

DawnOfTermina é um projeto independente, sem afiliação com a Nintendo. *The Legend of Zelda* e *Majora's Mask* são marcas registradas da Nintendo. Este repositório não distribui ROMs, binários, assets ou código proprietário; o projeto funciona exclusivamente no modelo *bring-your-own-ROM*, exigindo uma cópia legalmente adquirida do jogo. Não use este projeto para pirataria.
