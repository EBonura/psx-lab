#!/usr/bin/env python3
"""Convert PICO-8 Celeste audio data (.p8) to PS1 SPU C headers.

Parses __sfx__ and __music__ sections, generates:
  - sfx_data.h: note data + metadata for all 64 SFX entries
  - music_data.h: music pattern data
  - waveform_data.h: 8 ADPCM waveform loops for SPU RAM

Also pre-computes a pitch lookup table (PICO-8 key -> SPU sample rate register).
"""

import math
import os
import struct

# --- PICO-8 SFX/Music parsing ---

def parse_p8_audio(filename):
    """Parse __sfx__ and __music__ sections from a .p8 file."""
    sfx_lines = []
    music_lines = []
    section = None

    with open(filename) as f:
        for line in f:
            line = line.strip()
            if line == "__sfx__":
                section = "sfx"
                continue
            elif line == "__music__":
                section = "music"
                continue
            elif line.startswith("__"):
                section = None
                continue

            if section == "sfx" and len(line) >= 168:
                sfx_lines.append(line)
            elif section == "music" and len(line) >= 10:
                music_lines.append(line)

    return sfx_lines, music_lines


def decode_sfx(sfx_lines):
    """Decode SFX lines into structured data.

    .p8 SFX format per line (168 hex chars):
      - 2 chars: editor mode
      - 2 chars: speed
      - 2 chars: loop start
      - 2 chars: loop end
      - 32 notes x 5 chars each: PPIVF
        PP = pitch (2 hex digits, value 0-63)
        I  = instrument/waveform (0-7)
        V  = volume (0-7)
        F  = effect (0-7)
    """
    sfx_data = []
    for line in sfx_lines:
        editor_mode = int(line[0:2], 16)
        speed = int(line[2:4], 16)
        loop_start = int(line[4:6], 16)
        loop_end = int(line[6:8], 16)

        notes = []
        for i in range(32):
            offset = 8 + i * 5
            s = line[offset:offset + 5]
            pitch = int(s[0:2], 16)
            instrument = int(s[2], 16) & 7
            volume = int(s[3], 16) & 7
            effect = int(s[4], 16) & 7
            notes.append((pitch, instrument, volume, effect))

        sfx_data.append({
            "editor_mode": editor_mode,
            "speed": speed,
            "loop_start": loop_start,
            "loop_end": loop_end,
            "notes": notes,
        })

    # Pad to 64 entries
    while len(sfx_data) < 64:
        sfx_data.append({
            "editor_mode": 0, "speed": 1, "loop_start": 0, "loop_end": 0,
            "notes": [(0, 0, 0, 0)] * 32,
        })

    return sfx_data


def decode_music(music_lines):
    """Decode music pattern lines.

    Format per line: "FF AABBCCDD"
      FF = flags (hex byte): bit0=loop_start, bit1=loop_end, bit2=stop
      AA,BB,CC,DD = channel SFX indices (hex bytes)
        bit 6 (0x40) set = channel disabled
        bits 0-5 = SFX index
    """
    patterns = []
    for line in music_lines:
        parts = line.split()
        flags = int(parts[0], 16)
        channels_hex = parts[1]
        channels = []
        for i in range(4):
            val = int(channels_hex[i * 2:i * 2 + 2], 16)
            enabled = (val & 0x40) == 0
            sfx_idx = val & 0x3F
            channels.append((sfx_idx, enabled))
        patterns.append({"flags": flags, "channels": channels})

    # Pad to 64
    while len(patterns) < 64:
        patterns.append({
            "flags": 0,
            "channels": [(0, False), (0, False), (0, False), (0, False)],
        })

    return patterns


# --- ADPCM encoding ---

# PS1 ADPCM filter coefficients (fixed-point, /64)
ADPCM_K0 = [0, 60, 115, 98, 122]
ADPCM_K1 = [0, 0, -52, -55, -60]


def encode_adpcm_block(samples_28, flags=0):
    """Encode 28 PCM samples (int16) into one 16-byte PS1 ADPCM block.

    Tries all shift/filter combos, picks the one with lowest error.
    Returns 16 bytes.
    """
    best_error = float("inf")
    best_block = None

    for filt in range(4):
        k0 = ADPCM_K0[filt]
        k1 = ADPCM_K1[filt]

        for shift in range(13):
            error = 0
            old = 0
            older = 0
            nibbles = []

            for s in samples_28:
                pred = (old * k0 + older * k1 + 32) >> 6
                residual = s - pred
                # Quantize
                raw = residual >> (12 - shift) if shift <= 12 else residual << (shift - 12)
                nib = max(-8, min(7, round(raw)))
                # Decode back
                decoded = (nib << (12 - shift)) + pred
                decoded = max(-32768, min(32767, decoded))
                error += (decoded - s) ** 2
                nibbles.append(nib & 0xF)
                older = old
                old = decoded

            if error < best_error:
                best_error = error
                # Pack nibbles
                block = bytearray(16)
                block[0] = (shift & 0xF) | ((filt & 0x7) << 4)
                block[1] = flags
                for i in range(0, 28, 2):
                    lo = nibbles[i] & 0xF
                    hi = nibbles[i + 1] & 0xF if i + 1 < 28 else 0
                    block[2 + i // 2] = lo | (hi << 4)
                best_block = bytes(block)

    return best_block


# --- Waveform generation ---

def gen_waveform_samples(waveform_id, num_samples=28):
    """Generate one cycle of a PICO-8 waveform as int16 PCM samples."""
    amp = 24000  # Leave headroom below int16 max

    # Noise: use iterative LCG (not the per-sample t-based approach)
    if waveform_id == 6:
        samples = []
        seed = 12345
        for _ in range(num_samples):
            seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
            v = ((seed >> 16) & 0xFFFF) / 32768.0 - 1.0
            samples.append(int(max(-32768, min(32767, v * amp))))
        return samples

    samples = []
    n = num_samples
    for i in range(n):
        t = i / n  # 0.0 to 1.0

        if waveform_id == 0:  # Triangle
            if t < 0.5:
                v = t * 2.0
            else:
                v = 2.0 - t * 2.0
            v = v * 2.0 - 1.0

        elif waveform_id == 1:  # Tilted saw (steeper rise)
            if t < 0.875:
                v = t / 0.875
            else:
                v = 1.0 - (t - 0.875) / 0.125
            v = v * 2.0 - 1.0

        elif waveform_id == 2:  # Saw
            v = t * 2.0 - 1.0

        elif waveform_id == 3:  # Square (50% duty)
            v = 1.0 if t < 0.5 else -1.0

        elif waveform_id == 4:  # Pulse (25% duty)
            v = 1.0 if t < 0.25 else -1.0

        elif waveform_id == 5:  # Organ (triangle + harmonics)
            v = (math.sin(2 * math.pi * t)
                 + 0.5 * math.sin(4 * math.pi * t)
                 + 0.25 * math.sin(8 * math.pi * t)) / 1.75

        elif waveform_id == 7:  # Phaser (detuned square waves)
            sq1 = 1.0 if t < 0.5 else -1.0
            t2 = (t + 0.25) % 1.0
            sq2 = 1.0 if t2 < 0.5 else -1.0
            v = (sq1 + sq2 * 0.5) / 1.5

        else:
            v = 0.0

        samples.append(int(max(-32768, min(32767, v * amp))))

    return samples


def gen_all_waveforms():
    """Generate ADPCM data for all 8 waveforms.

    Each waveform = 2 ADPCM blocks (32 bytes):
      Block 0: loop_start flag (0x04), waveform data
      Block 1: loop_end + repeat flags (0x03), more waveform / padding

    For noise, use 8 blocks (224 samples) for a less tonal loop.
    """
    all_data = bytearray()

    for w in range(8):
        if w == 6:
            # Noise: 8 blocks = 224 samples for less tonal looping
            noise_samples = gen_waveform_samples(6, 224)
            blks = bytearray()
            for b in range(8):
                chunk = noise_samples[b * 28:(b + 1) * 28]
                if b == 0:
                    flags = 0x04  # loop start
                elif b == 7:
                    flags = 0x03  # loop end + repeat
                else:
                    flags = 0x00
                blks += encode_adpcm_block(chunk, flags=flags)
            all_data += blks
        else:
            # 2 blocks per waveform (56 samples = ~2 cycles at 28 samples/cycle)
            samples = gen_waveform_samples(w, 56)
            blk0 = encode_adpcm_block(samples[0:28], flags=0x04)   # loop start
            blk1 = encode_adpcm_block(samples[28:56], flags=0x03)  # loop end + repeat
            all_data += blk0 + blk1

    return all_data


# --- Pitch table ---

def gen_pitch_table(samples_per_cycle=56):
    """Pre-compute SPU sample rate register values for all 64 PICO-8 keys.

    PICO-8: freq = 440.0 * 2^((key - 33) / 12)
    SPU: rate_reg = (freq * samples_per_cycle / 44100) * 0x1000

    For noise waveform (224 samples/cycle), caller should quarter the pitch value.
    """
    table = []
    for key in range(64):
        freq = 440.0 * (2.0 ** ((key - 33) / 12.0))
        rate = int((freq * samples_per_cycle / 44100.0) * 0x1000 + 0.5)
        rate = max(0, min(0x3FFF, rate))
        table.append(rate)
    return table


# --- Output ---

def write_sfx_header(f, sfx_data):
    """Write sfx_data.h content."""
    f.write("#pragma once\n#include <stdint.h>\n\n")

    # SFX note: pack pitch(6) + instrument(3) + volume(3) + effect(3) = 15 bits into uint16_t
    f.write("// Packed SFX note: bits [5:0]=pitch, [8:6]=instrument, [11:9]=volume, [14:12]=effect\n")
    f.write("// Access macros:\n")
    f.write("#define SFX_PITCH(n)  ((n) & 0x3F)\n")
    f.write("#define SFX_INSTR(n)  (((n) >> 6) & 0x7)\n")
    f.write("#define SFX_VOL(n)    (((n) >> 9) & 0x7)\n")
    f.write("#define SFX_EFFECT(n) (((n) >> 12) & 0x7)\n\n")

    f.write("struct P8SfxMeta {\n")
    f.write("    uint8_t speed;\n")
    f.write("    uint8_t loop_start;\n")
    f.write("    uint8_t loop_end;\n")
    f.write("    uint8_t pad;\n")
    f.write("};\n\n")

    # SFX metadata
    f.write("static const P8SfxMeta sfx_meta[64] = {\n")
    for s in sfx_data:
        f.write(f"    {{{s['speed']}, {s['loop_start']}, {s['loop_end']}, 0}},\n")
    f.write("};\n\n")

    # SFX note data: 64 SFX x 32 notes
    f.write("static const uint16_t sfx_notes[64][32] = {\n")
    for si, s in enumerate(sfx_data):
        f.write(f"    {{ // SFX {si}\n        ")
        vals = []
        for pitch, instr, vol, effect in s["notes"]:
            packed = (pitch & 0x3F) | ((instr & 7) << 6) | ((vol & 7) << 9) | ((effect & 7) << 12)
            vals.append(f"0x{packed:04X}")
        f.write(", ".join(vals[:16]) + ",\n        ")
        f.write(", ".join(vals[16:]) + "\n")
        f.write("    },\n")
    f.write("};\n\n")


def write_music_header(f, patterns):
    """Write music_data.h content."""
    f.write("#pragma once\n#include <stdint.h>\n\n")

    f.write("// Music pattern flags\n")
    f.write("#define MUSIC_LOOP_START 0x01\n")
    f.write("#define MUSIC_LOOP_END   0x02\n")
    f.write("#define MUSIC_STOP       0x04\n\n")

    f.write("struct P8MusicPattern {\n")
    f.write("    uint8_t flags;\n")
    f.write("    uint8_t channel_sfx[4];  // SFX index, bit 7 = disabled\n")
    f.write("    uint8_t pad[3];\n")
    f.write("};\n\n")

    f.write("static const P8MusicPattern music_patterns[64] = {\n")
    for i, p in enumerate(patterns):
        chans = []
        for sfx_idx, enabled in p["channels"]:
            val = sfx_idx if enabled else (sfx_idx | 0x80)
            chans.append(f"0x{val:02X}")
        ch_str = ", ".join(chans)
        f.write(f"    {{0x{p['flags']:02X}, {{{ch_str}}}, {{0,0,0}}}},\n")
    f.write("};\n\n")

    # Count valid patterns (non-empty)
    num_valid = 0
    for p in patterns:
        has_active = any(en for _, en in p["channels"])
        if has_active or p["flags"]:
            num_valid = patterns.index(p) + 1
    # Find the last non-empty pattern
    for i in range(len(patterns) - 1, -1, -1):
        has_active = any(en for _, en in patterns[i]["channels"])
        if has_active or patterns[i]["flags"]:
            num_valid = i + 1
            break
    f.write(f"#define MUSIC_PATTERN_COUNT {num_valid}\n")


def write_waveform_header(f, adpcm_data, pitch_table):
    """Write waveform_data.h content."""
    f.write("#pragma once\n#include <stdint.h>\n\n")

    # Waveform ADPCM data
    f.write(f"// 8 ADPCM waveform loops ({len(adpcm_data)} bytes total)\n")
    f.write("// Waveforms 0-5,7: 2 blocks (32 bytes each)\n")
    f.write("// Waveform 6 (noise): 8 blocks (128 bytes)\n")
    f.write(f"static const uint8_t waveform_adpcm[{len(adpcm_data)}] __attribute__((aligned(4))) = {{\n")
    for i in range(0, len(adpcm_data), 16):
        chunk = adpcm_data[i:i + 16]
        f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",\n")
    f.write("};\n\n")

    # Byte offsets for each waveform in the ADPCM data
    offsets = []
    pos = 0
    for w in range(8):
        offsets.append(pos)
        pos += 128 if w == 6 else 32  # noise is 8 blocks, others 2
    f.write("// Byte offset of each waveform in waveform_adpcm[]\n")
    f.write("static const uint16_t waveform_offset[8] = {\n    ")
    f.write(", ".join(str(o) for o in offsets))
    f.write("\n};\n\n")

    # Byte size of each waveform
    sizes = [128 if w == 6 else 32 for w in range(8)]
    f.write("static const uint16_t waveform_size[8] = {\n    ")
    f.write(", ".join(str(s) for s in sizes))
    f.write("\n};\n\n")

    # Pitch lookup table
    f.write("// SPU sample rate register values for PICO-8 keys 0-63\n")
    f.write("// Based on 56 samples/cycle (for noise, quarter the value)\n")
    f.write("static const uint16_t spu_pitch_table[64] = {\n    ")
    for i in range(0, 64, 8):
        chunk = pitch_table[i:i + 8]
        f.write(", ".join(f"0x{v:04X}" for v in chunk))
        if i + 8 < 64:
            f.write(",\n    ")
    f.write("\n};\n\n")

    # SPU RAM base address for waveform uploads
    f.write("// SPU RAM address for waveform data upload (in bytes)\n")
    f.write("#define SPU_WAVEFORM_BASE 0x1000\n")


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    p8_file = os.path.join(script_dir, "..", "..", "picotron", "celeste_classic_audio.p8")
    out_dir = os.path.join(script_dir, "..", "src", "celeste")

    # Also check the local picotron directory
    if not os.path.exists(p8_file):
        p8_file = "/Users/ebonura/Desktop/picotron/celeste_classic_audio.p8"

    print(f"Reading {p8_file}...")
    sfx_lines, music_lines = parse_p8_audio(p8_file)
    print(f"  Found {len(sfx_lines)} SFX entries, {len(music_lines)} music patterns")

    sfx_data = decode_sfx(sfx_lines)
    music_data = decode_music(music_lines)

    # Generate waveforms
    print("Generating ADPCM waveforms...")
    adpcm_data = gen_all_waveforms()
    print(f"  Total ADPCM data: {len(adpcm_data)} bytes")

    pitch_table = gen_pitch_table(56)

    # Write headers
    sfx_path = os.path.join(out_dir, "sfx_data.h")
    print(f"Writing {sfx_path}...")
    with open(sfx_path, "w") as f:
        write_sfx_header(f, sfx_data)

    music_path = os.path.join(out_dir, "music_data.h")
    print(f"Writing {music_path}...")
    with open(music_path, "w") as f:
        write_music_header(f, music_data)

    wave_path = os.path.join(out_dir, "waveform_data.h")
    print(f"Writing {wave_path}...")
    with open(wave_path, "w") as f:
        write_waveform_header(f, adpcm_data, pitch_table)

    print("Done!")


if __name__ == "__main__":
    main()
