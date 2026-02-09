#include "psx_audio.h"

#include <stdint.h>
#include <stddef.h>

#include "common/hardware/dma.h"
#include "common/hardware/spu.h"

#include "sfx_data.h"
#include "music_data.h"
#include "waveform_data.h"

// --- SPU voice allocation ---
// Voices 0-3: music channels
// Voices 4-7: SFX channels (round-robin)
static constexpr int MUSIC_VOICE_BASE = 0;
static constexpr int SFX_VOICE_BASE = 4;
static constexpr int NUM_SFX_VOICES = 4;

// --- SPU helpers ---

static void spu_upload(uint32_t spu_addr, const void* data, uint32_t size) {
    uint32_t bcr = size >> 6;
    if (size & 0x3F) bcr++;
    bcr = (bcr << 16) | 0x10;

    SPU_RAM_DTA = spu_addr >> 3;
    SPU_CTRL = (SPU_CTRL & ~0x0030) | 0x0020;
    while ((SPU_CTRL & 0x0030) != 0x0020)
        ;

    SBUS_DEV4_CTRL &= ~0x0f000000;
    DMA_CTRL[DMA_SPU].MADR = (uintptr_t)data;
    DMA_CTRL[DMA_SPU].BCR = bcr;
    DMA_CTRL[DMA_SPU].CHCR = 0x01000201;
    while ((DMA_CTRL[DMA_SPU].CHCR & 0x01000000) != 0)
        ;
}

static void spu_key_on(uint32_t voice_bits) {
    SPU_KEY_ON_LOW = voice_bits & 0xFFFF;
    SPU_KEY_ON_HIGH = voice_bits >> 16;
}

static void spu_key_off(uint32_t voice_bits) {
    SPU_KEY_OFF_LOW = voice_bits & 0xFFFF;
    SPU_KEY_OFF_HIGH = voice_bits >> 16;
}

// --- Channel state ---

struct Channel {
    int sfx_id;          // -1 = inactive
    int note_pos;        // current note index (0-31)
    int tick;            // fractional accumulator (fixed-point 24.8)
    int vibrato_phase;   // vibrato LFO phase
    bool keyed_on;       // true if this voice is currently sounding
};

// 4 music channels + 4 SFX channels
static Channel channels[8];

// Music state
static int music_pattern = -1;    // current pattern index (-1 = stopped)
static int music_loop_start = -1; // pattern index to loop back to

// SFX round-robin counter
static int sfx_next_voice = 0;

// SPU RAM addresses for each waveform (in 8-byte units, as used by SPU registers)
static uint16_t waveform_spu_addr[8];

// --- Tick timing ---
// PICO-8 note duration: speed * 183 samples at 22050 Hz
//   = speed * 183/22050 seconds
//   = speed * 0.498 frames at 60fps
//
// We use fixed-point (8 fractional bits):
//   Each frame: tick += 256 (= 1.0)
//   Note threshold: speed * 128 (= speed * 0.5)
//   This gives speed 1 = 0.5 frames/note, speed 2 = 1 frame/note, etc.

static constexpr int TICK_INC = 256;         // added per frame (1.0 in fp8)
static constexpr int TICK_PER_SPEED = 128;   // 0.498 * 256 ≈ 128

// --- Volume mapping ---
// PICO-8 volume 0-7 -> SPU volume 0-0x3FFF
static const uint16_t vol_table[8] = {
    0x0000, 0x0800, 0x1000, 0x1800,
    0x2000, 0x2800, 0x3000, 0x3800,
};

// --- Internal functions ---

static uint16_t get_waveform_addr(int instr) {
    return waveform_spu_addr[instr & 7];
}

static uint16_t get_pitch(int key, int instr) {
    uint16_t p = spu_pitch_table[key & 63];
    // Noise uses 224 samples/cycle (4x base), so quarter the pitch
    if (instr == 6) p >>= 2;
    return p;
}

static void voice_key_off(int voice, Channel& ch) {
    if (ch.keyed_on) {
        spu_key_off(1 << voice);
        ch.keyed_on = false;
    }
}

static void start_channel_note(int voice, Channel& ch) {
    uint16_t note = sfx_notes[ch.sfx_id][ch.note_pos];
    int vol = SFX_VOL(note);
    int pitch_key = SFX_PITCH(note);
    int instr = SFX_INSTR(note);

    if (vol == 0) {
        voice_key_off(voice, ch);
        return;
    }

    uint16_t spu_vol = vol_table[vol];
    uint16_t spu_pitch = get_pitch(pitch_key, instr);
    uint16_t addr = get_waveform_addr(instr);

    // Key off first for clean restart
    voice_key_off(voice, ch);

    SPU_VOICES[voice].volumeLeft = spu_vol;
    SPU_VOICES[voice].volumeRight = spu_vol;
    SPU_VOICES[voice].sampleRate = spu_pitch;
    SPU_VOICES[voice].sampleStartAddr = addr;
    spu_key_on(1 << voice);
    ch.keyed_on = true;
}

static void apply_effects(int voice, Channel& ch) {
    if (ch.sfx_id < 0) return;

    uint16_t note = sfx_notes[ch.sfx_id][ch.note_pos];
    int effect = SFX_EFFECT(note);
    int pitch_key = SFX_PITCH(note);
    int instr = SFX_INSTR(note);
    int vol = SFX_VOL(note);

    if (vol == 0 || effect == 0) return;

    uint16_t base_pitch = get_pitch(pitch_key, instr);
    int speed = sfx_meta[ch.sfx_id].speed;
    int total = speed * TICK_PER_SPEED;
    if (total < 1) total = 1;
    int t = ch.tick;  // progress within this note

    switch (effect) {
    case 1: {  // slide — pitch toward next note
        int next_pos = ch.note_pos + 1;
        if (next_pos < 32) {
            uint16_t nn = sfx_notes[ch.sfx_id][next_pos];
            uint16_t target = get_pitch(SFX_PITCH(nn), SFX_INSTR(nn));
            int p = (int)base_pitch + (((int)target - (int)base_pitch) * t) / total;
            if (p < 1) p = 1;
            if (p > 0x3FFF) p = 0x3FFF;
            SPU_VOICES[voice].sampleRate = (uint16_t)p;
        }
        break;
    }

    case 2: {  // vibrato
        ch.vibrato_phase += 16;
        int phase = ch.vibrato_phase & 0xFF;
        int mod;
        if (phase < 64) mod = phase;
        else if (phase < 192) mod = 128 - phase;
        else mod = phase - 256;
        int p = (int)base_pitch + (mod * (int)base_pitch) / 2048;
        if (p < 1) p = 1;
        if (p > 0x3FFF) p = 0x3FFF;
        SPU_VOICES[voice].sampleRate = (uint16_t)p;
        break;
    }

    case 3: {  // drop — pitch drops to zero
        int p = (int)base_pitch * (total - t) / total;
        if (p < 0) p = 0;
        SPU_VOICES[voice].sampleRate = (uint16_t)p;
        break;
    }

    case 4: {  // fade_in
        uint16_t v = (uint16_t)((uint32_t)vol_table[vol] * t / total);
        SPU_VOICES[voice].volumeLeft = v;
        SPU_VOICES[voice].volumeRight = v;
        break;
    }

    case 5: {  // fade_out
        uint16_t v = (uint16_t)((uint32_t)vol_table[vol] * (total - t) / total);
        SPU_VOICES[voice].volumeLeft = v;
        SPU_VOICES[voice].volumeRight = v;
        break;
    }

    case 6: {  // arpeggio fast
        int step = (t / 4) % 3;
        int offset = (step == 0) ? 0 : (step == 1) ? 4 : 7;
        SPU_VOICES[voice].sampleRate = get_pitch((pitch_key + offset) & 63, instr);
        break;
    }

    case 7: {  // arpeggio slow
        int step = (t / 8) % 3;
        int offset = (step == 0) ? 0 : (step == 1) ? 4 : 7;
        SPU_VOICES[voice].sampleRate = get_pitch((pitch_key + offset) & 63, instr);
        break;
    }
    }
}

static void advance_channel(int voice, Channel& ch) {
    if (ch.sfx_id < 0) return;

    int speed = sfx_meta[ch.sfx_id].speed;
    if (speed < 1) speed = 1;
    int threshold = speed * TICK_PER_SPEED;

    ch.tick += TICK_INC;

    // Advance through notes (may skip multiple for fast SFX)
    while (ch.tick >= threshold) {
        ch.tick -= threshold;
        ch.note_pos++;
        ch.vibrato_phase = 0;

        // Handle looping
        int loop_end = sfx_meta[ch.sfx_id].loop_end;
        int loop_start = sfx_meta[ch.sfx_id].loop_start;
        if (loop_end > 0 && ch.note_pos >= loop_end) {
            ch.note_pos = loop_start;
        }

        if (ch.note_pos >= 32) {
            // SFX finished
            ch.sfx_id = -1;
            voice_key_off(voice, ch);
            return;
        }

        start_channel_note(voice, ch);
    }

    // Apply per-frame effects for the current note
    apply_effects(voice, ch);
}

// --- Music playback ---

static void music_advance_pattern() {
    if (music_pattern < 0 || music_pattern >= MUSIC_PATTERN_COUNT) {
        music_pattern = -1;
        return;
    }

    const P8MusicPattern& pat = music_patterns[music_pattern];

    if (pat.flags & MUSIC_LOOP_START) {
        music_loop_start = music_pattern;
    }

    for (int c = 0; c < 4; c++) {
        int voice = MUSIC_VOICE_BASE + c;
        Channel& ch = channels[c];

        if (pat.channel_sfx[c] & 0x80) {
            // Channel disabled
            if (ch.sfx_id >= 0) {
                ch.sfx_id = -1;
                voice_key_off(voice, ch);
            }
            continue;
        }

        int sfx_id = pat.channel_sfx[c] & 0x3F;
        ch.sfx_id = sfx_id;
        ch.note_pos = 0;
        ch.tick = 0;
        ch.vibrato_phase = 0;
        start_channel_note(voice, ch);
    }
}

static bool music_any_channel_done() {
    const P8MusicPattern& pat = music_patterns[music_pattern];
    for (int c = 0; c < 4; c++) {
        if (pat.channel_sfx[c] & 0x80) continue;
        if (channels[c].sfx_id < 0) return true;
    }
    return false;
}

// --- Public API ---

void audio_init() {
    DPCR |= 0x000b0000;

    SPU_VOL_MAIN_LEFT = 0x3800;
    SPU_VOL_MAIN_RIGHT = 0x3800;
    SPU_CTRL = 0;
    SPU_KEY_ON_LOW = 0;
    SPU_KEY_ON_HIGH = 0;
    SPU_KEY_OFF_LOW = 0xFFFF;
    SPU_KEY_OFF_HIGH = 0xFFFF;
    SPU_RAM_DTC = 4;
    SPU_VOL_CD_LEFT = 0;
    SPU_VOL_CD_RIGHT = 0;
    SPU_PITCH_MOD_LOW = 0;
    SPU_PITCH_MOD_HIGH = 0;
    SPU_NOISE_EN_LOW = 0;
    SPU_NOISE_EN_HIGH = 0;
    SPU_REVERB_EN_LOW = 0;
    SPU_REVERB_EN_HIGH = 0;
    SPU_VOL_EXT_LEFT = 0;
    SPU_VOL_EXT_RIGHT = 0;
    SPU_CTRL = 0x8000;

    for (int i = 0; i < 24; i++) {
        SPU_VOICES[i].volumeLeft = 0;
        SPU_VOICES[i].volumeRight = 0;
        SPU_VOICES[i].sampleRate = 0;
        SPU_VOICES[i].sampleStartAddr = 0;
        SPU_VOICES[i].ad = 0x000F;   // fastest attack, fastest decay, max sustain level
        SPU_VOICES[i].sr = 0x0000;   // fastest release
        SPU_VOICES[i].currentVolume = 0;
        SPU_VOICES[i].sampleRepeatAddr = 0;
    }

    spu_upload(SPU_WAVEFORM_BASE, waveform_adpcm, sizeof(waveform_adpcm));

    for (int w = 0; w < 8; w++) {
        waveform_spu_addr[w] = (SPU_WAVEFORM_BASE + waveform_offset[w]) >> 3;
    }

    for (int i = 0; i < 8; i++) {
        channels[i].sfx_id = -1;
        channels[i].note_pos = 0;
        channels[i].tick = 0;
        channels[i].vibrato_phase = 0;
        channels[i].keyed_on = false;
    }

    music_pattern = -1;
    music_loop_start = -1;

    SPU_CTRL = 0xC000;
}

void audio_update() {
    if (music_pattern >= 0) {
        for (int c = 0; c < 4; c++) {
            advance_channel(MUSIC_VOICE_BASE + c, channels[c]);
        }

        if (music_any_channel_done()) {
            const P8MusicPattern& pat = music_patterns[music_pattern];

            if (pat.flags & MUSIC_STOP) {
                music_pattern = -1;
                for (int c = 0; c < 4; c++) {
                    channels[c].sfx_id = -1;
                    voice_key_off(MUSIC_VOICE_BASE + c, channels[c]);
                }
            } else if (pat.flags & MUSIC_LOOP_END) {
                music_pattern = (music_loop_start >= 0) ? music_loop_start : 0;
                music_advance_pattern();
            } else {
                music_pattern++;
                if (music_pattern >= MUSIC_PATTERN_COUNT) {
                    music_pattern = -1;
                } else {
                    music_advance_pattern();
                }
            }
        }
    }

    for (int s = 0; s < NUM_SFX_VOICES; s++) {
        advance_channel(SFX_VOICE_BASE + s, channels[4 + s]);
    }
}

void audio_sfx(int id) {
    if (id < 0 || id >= 64) {
        for (int s = 0; s < NUM_SFX_VOICES; s++) {
            channels[4 + s].sfx_id = -1;
            voice_key_off(SFX_VOICE_BASE + s, channels[4 + s]);
        }
        return;
    }

    int voice_slot = -1;
    for (int s = 0; s < NUM_SFX_VOICES; s++) {
        if (channels[4 + s].sfx_id < 0) {
            voice_slot = s;
            break;
        }
    }
    if (voice_slot < 0) {
        voice_slot = sfx_next_voice;
        sfx_next_voice = (sfx_next_voice + 1) % NUM_SFX_VOICES;
    }

    int voice = SFX_VOICE_BASE + voice_slot;
    Channel& ch = channels[4 + voice_slot];
    ch.sfx_id = id;
    ch.note_pos = 0;
    ch.tick = 0;
    ch.vibrato_phase = 0;
    start_channel_note(voice, ch);
}

void audio_music(int pattern, int fade, int mask) {
    (void)fade;
    (void)mask;

    if (pattern < 0) {
        music_pattern = -1;
        for (int c = 0; c < 4; c++) {
            channels[c].sfx_id = -1;
            voice_key_off(MUSIC_VOICE_BASE + c, channels[c]);
        }
        return;
    }

    if (pattern >= MUSIC_PATTERN_COUNT) return;

    music_pattern = pattern;
    music_loop_start = -1;
    music_advance_pattern();
}
