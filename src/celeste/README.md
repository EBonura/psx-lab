# Celeste Classic for PlayStation 1

A port of PICO-8 Celeste Classic to the original PlayStation, using the [PSYQo](https://github.com/grumpycoders/pcsx-redux/tree/main/src/mips/psyqo) C++20 SDK.

The game logic comes from [ccleste](https://github.com/lemon32767/ccleste) (MIT license), a line-by-line C translation of the original PICO-8 Lua source by Maddy Thorson and Noel Berry. The platform layer (rendering, input, audio) is written from scratch to target PS1 hardware.

## Architecture

```
celeste.cpp        (game logic — unmodified from ccleste)
    │
    │  calls P8spr(), P8btn(), P8sfx(), etc.
    ▼
platform.h         (extern "C" interface — 14 PICO-8 API functions)
    │
    ▼
main.cpp           (PSYQo platform layer — rendering, input)
psx_audio.cpp      (SPU audio engine — music + SFX synthesis)
    │
    ▼
PS1 GPU / SPU      (hardware)
```

The game logic in `celeste.cpp` never touches hardware directly. It calls platform functions declared in `platform.h` (e.g. `P8spr`, `P8btn`, `P8music`), which are implemented in `main.cpp` and `psx_audio.cpp` using PSYQo primitives and direct SPU register programming.

## Files

| File | Description |
|------|-------------|
| `celeste.cpp` | Game logic from ccleste (unmodified, compiled as C++) |
| `celeste.h` | Game API: init, update, draw, state save/load |
| `tilemap.h` | Map data + tile flags (from ccleste) |
| `platform.h` | Platform function declarations (14 PICO-8 API calls) |
| `main.cpp` | PSYQo app + scene — rendering, input, VRAM management |
| `psx_audio.cpp` | SPU audio engine — real-time PICO-8 music/SFX synthesis |
| `psx_audio.h` | Audio API: init, update, sfx, music |
| `compat.cpp` | Bare-metal libc glue (memcpy, snprintf, no-op printf) |
| `libc/` | Stub headers so celeste.cpp compiles without a full libc |
| `gfx_data.h` | Generated — spritesheet as PS1 4bpp pixel data |
| `font_data.h` | Generated — font as PS1 4bpp pixel data |
| `sfx_data.h` | Generated — 64 SFX entries (notes + metadata) |
| `music_data.h` | Generated — 42 music patterns |
| `waveform_data.h` | Generated — 8 ADPCM waveform loops + pitch table |
| `data/gfx.bmp` | Source spritesheet from Celeste Classic |
| `data/font.bmp` | Source font bitmap |
| `Makefile` | Build config |

## Rendering

- **Resolution**: 256x240 (PS1 native), 2x scale of PICO-8's 128x128
- **Sprites/map**: 4bpp textures in VRAM, drawn as `Prim::Sprite` fragments
- **Shapes**: `Prim::Rectangle` (rectfill, circfill spans), `Prim::Line`
- **Text**: Per-character sprites from the font texture
- **Palette**: 16-entry CLUT in VRAM, swapped at runtime for `pal()` calls
- **Double-buffered**: Two framebuffers, two ordering tables, two fragment pools

## Audio

Music and sound effects are synthesized in real-time by the PS1 SPU, not pre-recorded. The 8 PICO-8 waveforms (triangle, tilted saw, saw, square, pulse, organ, noise, phaser) are encoded as short ADPCM loops and uploaded to SPU RAM at boot. Each frame, `audio_update()` advances note playback and programs SPU voice registers for pitch, volume, and effects.

- **Voices 0-3**: Music channels (4 simultaneous, following pattern sequencer)
- **Voices 4-7**: SFX channels (round-robin allocation)
- **Effects**: Slide, vibrato, drop, fade in/out, arpeggio (fast/slow)
- **Timing**: Fixed-point accumulator matching PICO-8's 183 samples/note at 22050 Hz

## Asset Pipeline

Two Python scripts in `tools/` convert source assets to PS1-ready C headers:

- **`convert_assets.py`** — Converts `gfx.bmp` and `font.bmp` to 4bpp pixel arrays + 15-bit CLUT
- **`convert_audio.py`** — Parses PICO-8 `.p8` audio sections, synthesizes 8 waveforms as ADPCM, pre-computes SPU pitch table, outputs SFX/music/waveform headers

To regenerate:
```sh
python3 tools/convert_assets.py
python3 tools/convert_audio.py
```

## Building

Requires a `mipsel-none-elf` cross-compiler (GCC 14+) and the PSYQo SDK (included as a submodule under `third_party/nugget`).

```sh
cd src/celeste
make deploy BUILD=Release
```

Produces `release/celeste.ps-exe` (~136 KB).

## Input Mapping

| PS1 | PICO-8 | Action |
|-----|--------|--------|
| D-pad | Arrows | Move |
| Cross | Z/C | Jump |
| Circle | X | Dash |

## Credits

- **Celeste Classic**: Maddy Thorson & Noel Berry (original PICO-8 game)
- **ccleste**: lemon32767 (C translation, MIT license)
- **PSYQo**: grumpycoders / Nicolas Noble (PS1 C++20 SDK)
- **nugget**: grumpycoders (bare-metal PS1 toolchain — SPU and DMA register definitions used for the audio engine)
