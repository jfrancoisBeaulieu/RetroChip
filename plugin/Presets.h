// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include "genesis/FmPatches.h"

#include <array>

namespace retrochip
{
// A preset sets the console, the controls of that console, and the shared vibrato and glide.
// The other console's controls keep their values.
struct Preset
{
    const char* name;
    int channel; // 0 pulse, 1 triangle, 2 noise
    int duty;    // 0 to 3
    int noiseMode; // 0 long, 1 short
    float attack, decay, sustain, release;
    float vibratoRate, vibratoDepth;
    float glide;
    int console = 0; // index of the console parameter: 0 is the 2A03, 1 the YM2612, 2 the S-DSP, 4 the SN76489
    // YM2612 only. Parameter values: genAlgorithm counts from 1.
    int genPatch = 0;
    int genAlgorithm = 5;
    int genFeedback = 0;
    float genBrightness = 0.0f, genAttack = 0.0f, genDecay = 0.0f, genRelease = 0.0f;
    // S-DSP only, as parameter values: the chip's steps for attack, decay and sustain, milliseconds
    // and percent for the rest. The root note and loop belong to the user's sample, so no preset sets them.
    int snesInstrument = 0;
    int snesAttack = 15;
    int snesDecay = 7;
    int snesSustainLevel = 7;
    int snesSustainRate = 0;
    float snesRelease = 8.0f;
    float snesEchoMix = 0.0f;
    int snesEchoDelay = 4;
    float snesEchoFeedback = 30.0f;
    // SN76489 only, as parameter values.
    int psgChannel = 0; // 0 tone, 1 noise
    int psgNoiseType = 0; // 0 white, 1 periodic
    int psgNoiseRate = 3; // 0 to 2 fixed, high to low; 3 follows the note
    float psgAttack = 0.0f, psgDecay = 0.0f, psgSustain = 15.0f, psgRelease = 50.0f;
    // VRC6 and N163 only. Their envelope is the 2A03's: attack, decay, sustain and release above.
    int vrc6Channel = 0; // 0 pulse, 1 saw
    int vrc6Duty = 7;
    int n163Wave = 0;
    int n163Channels = 1;
};

// A YM2612 preset. Name, algorithm and feedback come from the timbre, so the two can't drift apart.
constexpr Preset fmPreset(int patch, float vibratoRate, float vibratoDepth, float glide, float brightness = 0.0f,
                          float attack = 0.0f, float decay = 0.0f, float release = 0.0f)
{
    const auto& fm = kFmPatches[patch];
    Preset preset { fm.name, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, vibratoRate, vibratoDepth, glide, 1 };
    preset.genPatch = patch;
    preset.genAlgorithm = fm.algorithm + 1;
    preset.genFeedback = fm.feedback;
    preset.genBrightness = brightness;
    preset.genAttack = attack;
    preset.genDecay = decay;
    preset.genRelease = release;
    return preset;
}

// An S-DSP preset on a bank instrument: the instrument, the envelope, the echo, then vibrato and glide.
constexpr Preset sdspPreset(const char* name, int instrument, int attack, int decay, int sustainLevel, int sustainRate,
                            float release, float echoMix, int echoDelay, float echoFeedback, float vibratoRate,
                            float vibratoDepth, float glide)
{
    Preset preset { name, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, vibratoRate, vibratoDepth, glide, 2 };
    preset.snesInstrument = instrument;
    preset.snesAttack = attack;
    preset.snesDecay = decay;
    preset.snesSustainLevel = sustainLevel;
    preset.snesSustainRate = sustainRate;
    preset.snesRelease = release;
    preset.snesEchoMix = echoMix;
    preset.snesEchoDelay = echoDelay;
    preset.snesEchoFeedback = echoFeedback;
    return preset;
}

// An SN76489 preset: the channel, the envelope, then vibrato and glide.
constexpr Preset psgPreset(const char* name, int channel, int noiseType, int noiseRate, float attack, float decay, float sustain,
                           float release, float vibratoRate, float vibratoDepth, float glide)
{
    Preset preset { name, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, vibratoRate, vibratoDepth, glide, 4 };
    preset.psgChannel = channel;
    preset.psgNoiseType = noiseType;
    preset.psgNoiseRate = noiseRate;
    preset.psgAttack = attack;
    preset.psgDecay = decay;
    preset.psgSustain = sustain;
    preset.psgRelease = release;
    return preset;
}

// A VRC6 preset (console 5: channel and duty) or an N163 one (console 6: wave and channel count), then the envelope,
// vibrato and glide.
constexpr Preset cartridgePreset(const char* name, int console, int first, int second, float attack, float decay, float sustain,
                                 float release, float vibratoRate, float vibratoDepth, float glide)
{
    Preset preset { name, 0, 0, 0, attack, decay, sustain, release, vibratoRate, vibratoDepth, glide, console };
    if (console == 5)
    {
        preset.vrc6Channel = first;
        preset.vrc6Duty = second;
    }
    else
    {
        preset.n163Wave = first;
        preset.n163Channels = second;
    }
    return preset;
}

// The first preset matches the parameter defaults. Programs 0 to 11 are the 2A03's, 12 to 23 the YM2612's,
// 24 to 35 the S-DSP's, 36 to 47 the SN76489's, 48 to 59 the VRC6's, 60 to 71 the N163's.
inline constexpr std::array<Preset, 72> kPresets = { {
    { "Square Lead",  0, 2, 0,   0.0f,    0.0f, 15.0f,  50.0f, 5.0f,  0.0f,   0.0f },
    { "Thin Lead",    0, 0, 0,   0.0f,  120.0f, 11.0f,  80.0f, 5.5f,  8.0f,   0.0f },
    { "Reedy Lead",   0, 1, 0,  10.0f,   80.0f, 12.0f,  90.0f, 6.0f, 12.0f,   0.0f },
    { "Pluck Arp",    0, 1, 0,   0.0f,  180.0f,  0.0f,  30.0f, 5.0f,  0.0f,   0.0f },
    { "Soft Pad",     0, 2, 0, 400.0f,  300.0f,  9.0f, 500.0f, 4.5f, 10.0f,   0.0f },
    { "Glide Lead",   0, 3, 0,   0.0f,   60.0f, 13.0f,  70.0f, 5.5f,  6.0f, 120.0f },
    { "Deep Bass",    1, 2, 0,   0.0f,    0.0f, 15.0f,   0.0f, 5.0f,  0.0f,   0.0f },
    { "Flute Lead",   1, 2, 0,   0.0f,    0.0f, 15.0f,   0.0f, 5.0f, 15.0f,   0.0f },
    { "Sliding Bass", 1, 2, 0,   0.0f,    0.0f, 15.0f,   0.0f, 5.0f,  0.0f, 150.0f },
    { "Hi-Hat",       2, 2, 0,   0.0f,   70.0f,  0.0f,  20.0f, 5.0f,  0.0f,   0.0f },
    { "Snare",        2, 2, 0,   0.0f,  180.0f,  0.0f,  40.0f, 5.0f,  0.0f,   0.0f },
    { "Metal Hit",    2, 2, 1,   0.0f,  400.0f,  0.0f,  80.0f, 5.0f,  0.0f,   0.0f },

    // fmPreset(patch, vibrato rate, vibrato depth, glide, then optional brightness, attack, decay, release in %)
    fmPreset(0,       5.0f,    0.0f,    0.0f),             // Pure Tone
    fmPreset(1,       5.0f,    0.0f,    0.0f),             // FM Bass
    fmPreset(2,       5.0f,    0.0f,    0.0f),             // Slap Bass
    fmPreset(3,       5.0f,    0.0f,    0.0f),             // E-Piano
    fmPreset(4,       5.0f,    0.0f,    0.0f),             // Bell
    fmPreset(5,       5.5f,    8.0f,    0.0f),             // Brass
    fmPreset(6,       5.5f,   12.0f,   60.0f),             // Lead
    fmPreset(7,       6.0f,    6.0f,    0.0f),             // Organ
    fmPreset(8,       4.5f,   10.0f,    0.0f),             // Pad
    fmPreset(9,       5.0f,    0.0f,    0.0f),             // Guitar
    fmPreset(10,      5.0f,    0.0f,    0.0f),             // Pluck
    fmPreset(11,      5.0f,    0.0f,    0.0f),             // Perc

    // sdspPreset(name, instrument, attack, decay, sustain level, sustain rate, release ms,
    //            echo mix %, echo delay, echo feedback %, vibrato rate, vibrato depth, glide ms)
    sdspPreset("Warm Strings",   0,  9, 7, 7,  0,  350.0f, 20.0f,  6, 30.0f, 5.5f, 10.0f,  0.0f), // Strings
    sdspPreset("Brass Stab",     1, 12, 5, 4, 20,  120.0f, 10.0f,  3, 20.0f, 5.0f,  0.0f,  0.0f), // Brass
    sdspPreset("Church Organ",   2, 13, 7, 7,  0,  200.0f, 35.0f, 10, 45.0f, 6.0f,  6.0f,  0.0f), // Organ
    sdspPreset("Round Bass",     3, 15, 6, 6,  0,   80.0f,  0.0f,  4, 30.0f, 5.0f,  0.0f, 40.0f), // Bass
    sdspPreset("Temple Bell",    4, 15, 7, 7,  0,  600.0f, 40.0f,  8, 50.0f, 5.0f,  0.0f,  0.0f), // Bell
    sdspPreset("Breathy Flute",  5, 10, 7, 7,  0,  250.0f, 15.0f,  5, 25.0f, 5.5f, 14.0f,  0.0f), // Flute
    sdspPreset("Choir Hall",     6,  8, 7, 7,  0,  700.0f, 45.0f, 12, 55.0f, 4.5f,  8.0f,  0.0f), // Choir
    sdspPreset("Dream Piano",    7, 15, 7, 7,  0,  400.0f, 30.0f,  7, 40.0f, 5.0f,  0.0f,  0.0f), // Piano
    sdspPreset("Nylon Guitar",   8, 15, 7, 7,  0,  150.0f, 18.0f,  4, 30.0f, 5.0f,  0.0f,  0.0f), // Guitar
    sdspPreset("Marimba Echo",   9, 15, 7, 7,  0,  100.0f, 50.0f,  6, 60.0f, 5.0f,  0.0f,  0.0f), // Marimba
    sdspPreset("Reed Lead",     10, 12, 6, 6,  0,   90.0f, 12.0f,  3, 25.0f, 6.0f, 12.0f, 60.0f), // Reed
    sdspPreset("Slow Pad",      11,  6, 7, 7,  0, 1200.0f, 40.0f, 14, 50.0f, 4.0f,  6.0f,  0.0f), // Pad

    // psgPreset(name, channel, noise type, noise rate, attack, decay, sustain, release, vibrato rate, vibrato depth, glide)
    psgPreset("PSG Square",    0, 0, 3,   0.0f,   0.0f, 15.0f,  50.0f, 5.0f,  0.0f,   0.0f),
    psgPreset("PSG Soft Lead", 0, 0, 3,  10.0f, 120.0f, 11.0f,  90.0f, 5.5f, 10.0f,   0.0f),
    psgPreset("PSG Arp",       0, 0, 3,   0.0f, 160.0f,  0.0f,  30.0f, 5.0f,  0.0f,   0.0f),
    psgPreset("PSG Pad",       0, 0, 3, 350.0f, 300.0f,  9.0f, 500.0f, 4.5f, 10.0f,   0.0f),
    psgPreset("PSG Glide",     0, 0, 3,   0.0f,  60.0f, 13.0f,  70.0f, 5.5f,  6.0f, 120.0f),
    psgPreset("PSG Chime",     0, 0, 3,   0.0f, 500.0f,  0.0f, 200.0f, 5.0f,  0.0f,   0.0f),
    psgPreset("PSG Bass",      1, 1, 3,   0.0f,   0.0f, 15.0f,  40.0f, 5.0f,  0.0f,   0.0f),
    psgPreset("PSG Buzz",      1, 1, 3,   0.0f, 150.0f, 10.0f,  80.0f, 5.5f,  8.0f,   0.0f),
    psgPreset("PSG Hi-Hat",    1, 0, 0,   0.0f,  60.0f,  0.0f,  20.0f, 5.0f,  0.0f,   0.0f),
    psgPreset("PSG Snare",     1, 0, 1,   0.0f, 170.0f,  0.0f,  40.0f, 5.0f,  0.0f,   0.0f),
    psgPreset("PSG Crash",     1, 0, 0,   0.0f, 800.0f,  0.0f, 200.0f, 5.0f,  0.0f,   0.0f),
    psgPreset("PSG Tom",       1, 0, 3,   0.0f, 220.0f,  0.0f,  50.0f, 5.0f,  0.0f,   0.0f),

    // cartridgePreset(name, console, channel or wave, duty or channel count, attack, decay, sustain, release,
    //                 vibrato rate, vibrato depth, glide)
    cartridgePreset("VRC6 Square",     5, 0, 7,   0.0f,   0.0f, 15.0f,  50.0f, 5.0f,  0.0f,   0.0f),
    cartridgePreset("VRC6 Thin Lead",  5, 0, 1,   0.0f, 120.0f, 11.0f,  80.0f, 5.5f,  8.0f,   0.0f),
    cartridgePreset("VRC6 Reed",       5, 0, 3,  10.0f,  80.0f, 12.0f,  90.0f, 6.0f, 12.0f,   0.0f),
    cartridgePreset("VRC6 Soft Pad",   5, 0, 5, 400.0f, 300.0f,  9.0f, 500.0f, 4.5f, 10.0f,   0.0f),
    cartridgePreset("VRC6 Glide Lead", 5, 0, 2,   0.0f,  60.0f, 13.0f,  70.0f, 5.5f,  6.0f, 120.0f),
    cartridgePreset("VRC6 Pluck",      5, 0, 4,   0.0f, 180.0f,  0.0f,  30.0f, 5.0f,  0.0f,   0.0f),
    cartridgePreset("Saw Lead",        5, 1, 0,   0.0f,   0.0f, 15.0f,  60.0f, 5.5f, 10.0f,   0.0f),
    cartridgePreset("Saw Bass",        5, 1, 0,   0.0f,   0.0f, 15.0f,  30.0f, 5.0f,  0.0f,   0.0f),
    cartridgePreset("Saw Brass",       5, 1, 0,  40.0f, 200.0f, 11.0f, 120.0f, 5.5f,  8.0f,   0.0f),
    cartridgePreset("Saw Pad",         5, 1, 0, 500.0f, 400.0f, 10.0f, 600.0f, 4.5f, 12.0f,   0.0f),
    cartridgePreset("Saw Pluck",       5, 1, 0,   0.0f, 250.0f,  0.0f,  60.0f, 5.0f,  0.0f,   0.0f),
    cartridgePreset("Saw Slide",       5, 1, 0,   0.0f,   0.0f, 15.0f,  60.0f, 5.0f,  0.0f, 150.0f),

    cartridgePreset("N163 Sine",       6, 0, 1,   0.0f,   0.0f, 15.0f,  80.0f, 5.0f,  0.0f,   0.0f),
    cartridgePreset("N163 Flute",      6, 0, 1,  30.0f, 100.0f, 12.0f, 150.0f, 5.5f, 14.0f,   0.0f),
    cartridgePreset("N163 Soft Bass",  6, 1, 1,   0.0f,   0.0f, 15.0f,  40.0f, 5.0f,  0.0f,   0.0f),
    cartridgePreset("N163 Saw Lead",   6, 2, 1,   0.0f,  80.0f, 13.0f,  90.0f, 5.5f, 10.0f,   0.0f),
    cartridgePreset("N163 Square",     6, 3, 1,   0.0f,   0.0f, 15.0f,  50.0f, 5.0f,  0.0f,   0.0f),
    cartridgePreset("N163 Pulse Lead", 6, 4, 1,   0.0f,  60.0f, 13.0f,  70.0f, 5.5f,  8.0f,  60.0f),
    cartridgePreset("N163 Organ",      6, 5, 1,   0.0f,   0.0f, 15.0f, 120.0f, 6.0f,  6.0f,   0.0f),
    cartridgePreset("N163 Hollow",     6, 6, 1,  20.0f, 150.0f, 11.0f, 200.0f, 5.0f,  8.0f,   0.0f),
    cartridgePreset("N163 Bell",       6, 7, 1,   0.0f, 600.0f,  0.0f, 300.0f, 5.0f,  0.0f,   0.0f),
    cartridgePreset("N163 Grit Lead",  6, 2, 8,   0.0f,  80.0f, 13.0f,  90.0f, 5.5f, 10.0f,   0.0f),
    cartridgePreset("N163 Grit Bass",  6, 3, 8,   0.0f,   0.0f, 15.0f,  40.0f, 5.0f,  0.0f,   0.0f),
    cartridgePreset("N163 Pad",        6, 5, 4, 400.0f, 300.0f, 10.0f, 700.0f, 4.5f, 10.0f,   0.0f),
} };
} // namespace retrochip
