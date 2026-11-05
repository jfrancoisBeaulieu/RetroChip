// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include <algorithm>
#include <array>

// The S-DSP's ADSR envelope, from fullsnes ("SNES APU DSP ADSR/Gain Envelope"). Stepped at 32 kHz.
namespace retrochip::snes
{
// Samples at 32 kHz between steps, per rate. Rate 0 never steps.
inline constexpr std::array<int, 32> kEnvelopeRatePeriods = {
    0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
    64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1,
};

inline constexpr int kEnvelopeMax = 0x7FF;

// The linear release, in 1/65536 of a level per sample. 8 ms is the chip's own: -8 per sample.
inline constexpr int envelopeReleaseStep(double releaseMs)
{
    return static_cast<int>(kEnvelopeMax * 65536.0 / (releaseMs * 32.0));
}

struct EnvelopeSettings
{
    int attack = 15;      // 0 to 15, rate 2N + 1
    int decay = 7;        // 0 to 7, rate 2N + 16
    int sustainLevel = 7; // 0 to 7, boundary (N + 1) x 0x100
    int sustainRate = 0;  // 0 to 31
    int releaseStep = envelopeReleaseStep(8.0);
};

class SnesEnvelope
{
public:
    enum class Phase { Off, Attack, Decay, Sustain, Release };

    void start()
    {
        current = Phase::Attack;
        value = 0;
        counter = 0;
        fraction = 0;
    }

    // Goes to the release from any phase, as key-off does.
    void release()
    {
        if (current != Phase::Off)
            current = Phase::Release;
    }

    void stop()
    {
        current = Phase::Off;
        value = 0;
    }

    // One sample at 32 kHz. Reads the settings every time, so a change reaches a sounding note.
    void step(const EnvelopeSettings& settings)
    {
        switch (current)
        {
            case Phase::Attack:
            {
                const int rate = 2 * settings.attack + 1;
                if (!rateElapsed(rate))
                    break;
                value += rate == 31 ? 1024 : 32;
                if (value >= 0x7E0)
                {
                    current = Phase::Decay;
                    value = std::min(value, kEnvelopeMax);
                }
                break;
            }
            case Phase::Decay:
                if (value <= (settings.sustainLevel + 1) * 0x100)
                {
                    current = Phase::Sustain;
                    stepExponential(settings.sustainRate);
                }
                else
                {
                    stepExponential(2 * settings.decay + 16);
                }
                break;
            case Phase::Sustain:
                stepExponential(settings.sustainRate);
                break;
            case Phase::Release:
                fraction += settings.releaseStep;
                value -= fraction >> 16;
                fraction &= 0xFFFF;
                finishAtZero();
                break;
            case Phase::Off:
                break;
        }
    }

    int level() const { return value; }
    Phase phase() const { return current; }
    bool finished() const { return current == Phase::Off; }

private:
    bool rateElapsed(int rate)
    {
        const int period = kEnvelopeRatePeriods[static_cast<size_t>(rate)];
        if (period == 0 || ++counter < period)
            return false;
        counter = 0;
        return true;
    }

    void stepExponential(int rate)
    {
        if (!rateElapsed(rate))
            return;
        value -= ((value - 1) >> 8) + 1;
        finishAtZero();
    }

    void finishAtZero()
    {
        if (value <= 0)
            stop();
    }

    Phase current = Phase::Off;
    int value = 0;    // 11 bits
    int counter = 0;  // samples since the last step
    int fraction = 0; // of the release's step, in 1/65536
};
} // namespace retrochip::snes
