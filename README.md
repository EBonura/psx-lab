# psx-lab

PlayStation 1 homebrew experiments using [PSYQo](https://github.com/pcsx-redux/nugget/tree/main/psyqo) (C++20). The goal is to make PSX development as easy as possible to get started with.

## Prerequisites

- **mipsel-none-elf-gcc** — MIPS cross-compiler for PS1
- **GNU Make**
- **[DuckStation](https://github.com/stenzek/duckstation)** (or any PS1 emulator) for testing

### Installing the cross-compiler

macOS (Homebrew):
```bash
brew install ./third_party/nugget/mipsel-none-elf.rb
```

Linux:
```bash
# See https://github.com/pcsx-redux/nugget#readme for install instructions
```

## Getting started

```bash
git clone --recursive https://github.com/EBonura/psx-lab.git
cd psx-lab
make BUILD=Release
```

The compiled binary lands in `release/room_test.ps-exe` — open it in DuckStation to run.

## Project structure

```
psx-lab/
├── Makefile                # top-level build wrapper
├── src/
│   ├── Makefile            # build config (includes psyqo.mk)
│   └── room_test.cpp       # current experiment
├── third_party/
│   └── nugget/             # PSYQo library (git submodule)
└── release/                # build output (.ps-exe)
```

## What's PSYQo?

[PSYQo](https://github.com/pcsx-redux/nugget/tree/main/psyqo) is a modern C++20 library for PS1 development, part of the [nugget](https://github.com/pcsx-redux/nugget) toolchain from the PCSX-Redux project. It provides GPU, GTE, controller, and audio abstractions — no legacy Sony SDK required.

## Current experiment: room_test

A first-person room renderer with:

- World-space FPS camera with proper GTE view transform
- 5x5 tiled floor with checkerboard pattern
- Colored perimeter walls (north/south/east/west)
- D-pad movement (up/down = walk, left/right = turn)
- L1/R1 strafe, analog stick support
- Near-plane handling with vertex clamping
- Double-buffered rendering

### Controls

| Input | Action |
|-------|--------|
| D-pad Up/Down | Walk forward/backward |
| D-pad Left/Right | Turn |
| L1/R1 | Strafe left/right |
| Left stick | Look/turn |
| Right stick | Move/strafe |
