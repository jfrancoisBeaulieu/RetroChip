// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include "ConsoleEngine.h"
#include "Resampler.h"

#include <array>
#include <cstdint>

namespace retrochip
{
struct PsgParams
{
    bool noise = false;    // the noise channel rather than a tone
    bool periodic = false; // noise only: the 16-step buzz that plays notes, rather than white noise
    int noiseRate = 3;     // noise only: 0 to 2 are the chip's fixed rates, high to low, and 3 follows the note
    float attackMs = 0.0f;
    float decayMs = 0.0f;
    int sustain = 15;      // 0 to 15, in the chip's 2 dB steps. 0 ends the note after the decay.
    float releaseMs = 50.0f;
    float vibratoRateHz = 5.0f;
    float vibratoDepthCents = 0.0f;
    float glideMs = 0.0f;
    bool velocityOn = true;
};

// The Genesis's SN76489: square tones, or noise from a shift register, at 16 volumes. The chip has no
// envelope, so here as in a game's sound driver the level and the pitch change only at a 60 Hz step.
// The instance plays one channel type, on 8 voices, like the other chips. Each voice runs at the chip's
// rate, and a Resampler brings the mono sum to the host's rate.
//
// Tones are limited to the chip's range: below about 109 Hz (note 45) the pitch stays at its limit, and
// the top notes are as far out of tune as the 10-bit divider makes them. Periodic noise is 16 times
// lower than its rate, which is why games use it for bass, and it reaches the low notes.
//
// Large because of the resampler, so keep it off the stack. Nothing allocates after construction. Not copyable.
class PsgEngine final : public ConsoleEngine
{
public:
    static constexpr int kNumVoices = 8;
    static constexpr int kClock = 3579545;                 // NTSC, Hz
    static constexpr double kChipRate = kClock / 16.0;     // about 223722 Hz: one step of the dividers
    static constexpr int kMaxDivider = 1023;
    static constexpr int kNoiseFollowsNote = 3;

    // One step of the shift register: 16 bits, with bits 0 and 3 fed back for white noise, and bit 0
    // alone for periodic noise.
    static constexpr std::uint16_t nextNoise(std::uint16_t state, bool white)
    {
        const int feedback = white ? ((state ^ (state >> 3)) & 1) : (state & 1);
        return static_cast<std::uint16_t>((state >> 1) | (feedback << 15));
    }

    PsgEngine();

    void prepare(double sampleRate) override;
    void noteOn(int note, int velocity) override;
    void noteOff(int note) override;
    void setPitchBend(double semitones) override;
    void setSustain(bool on) override;
    void allNotesOff() override;
    void render(float* out, int numSamples) override;

    // Changing between tone and noise stops sounding notes. Other changes reach them at the next 60 Hz step.
    void setParams(const PsgParams& params);

    int activeVoices() const;
    bool isNoteActive(int note) const;

    // Delay through the resampler, in output samples.
    int latencySamples() const;

private:
    enum class Stage
    {
        attack,
        decay,
        sustain,
        release
    };

    struct Voice
    {
        bool active = false;
        bool sustained = false; // the player's key is up, held by the pedal
        Stage stage = Stage::attack;
        int note = 0;
        int velocity = 127;
        std::uint64_t age = 0;

        double envelope = 0.0;    // 0 to 1
        double releaseStep = 0.0; // per 60 Hz step
        double pitch = 0.0;
        double glideStep = 0.0;
        double vibratoPhase = 0.0;

        int divider = 1;
        int counter = 1;
        bool high = false;
        std::uint16_t noise = 0x8000;
        double level = 0.0;
    };

    int findVoiceFor(int note) const;
    void release(Voice& voice);
    void updatePitch(Voice& voice);
    void updateLevel(Voice& voice);
    void stepEnvelope(Voice& voice);
    void tick();
    void generate(float* out, int numSamples);

    std::array<Voice, kNumVoices> voices;
    std::array<double, 16> levels {};
    PsgParams params;
    Resampler resampler;
    std::array<float, Resampler::kMaxChunk> scratch {};

    bool prepared = false;
    bool sustainOn = false;
    double bend = 0.0;
    int lastNote = -1;
    std::uint64_t nextAge = 0;
    double samplesToTick = 0.0;
    double dcCoefficient = 1.0;
    double dcInput = 0.0;
    double dcOutput = 0.0;
};
} // namespace retrochip
