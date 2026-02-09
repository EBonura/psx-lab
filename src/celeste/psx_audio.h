#pragma once

// PS1 SPU audio engine for PICO-8 Celeste
// Synthesizes PICO-8 waveforms using SPU hardware voices.

void audio_init();    // Upload waveforms to SPU RAM, configure voices
void audio_update();  // Call once per frame — advances note playback, programs SPU
void audio_sfx(int id);                       // Trigger a SFX (0-63, -1 to stop all)
void audio_music(int pattern, int fade, int mask);  // Start/stop music
