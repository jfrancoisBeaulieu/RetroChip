// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "SoundFontEngine.h"

#include "SoundFontBank.h"

#include <algorithm>
#include <cmath>

namespace retrochip
{
namespace
{
constexpr double kBendRange = 2.0; // semitones each way, as the bank's channel is set
constexpr int kWheelCentre = 8192;
constexpr int kWheelMax = 16383;

// Brings a sample at full scale to the loudness of the other consoles. Measured on the test font's
// A4 sine at 80 % of full scale against a 2A03 pulse at full volume.
constexpr float kOutputGain = 0.25f;
} // namespace

SoundFontEngine::SoundFontEngine() { echo.setDelay(params.echoDelay); }

SoundFontEngine::~SoundFontEngine() = default;

void SoundFontEngine::prepare(double newSampleRate)
{
    if (newSampleRate <= 0.0)
        return;

    sampleRate = newSampleRate;
    echo.prepare(sampleRate);
    sustainOn = false;
    pitchWheel = kWheelCentre;
    if (bank != nullptr)
    {
        bank->stopNotes();
        bank->setSampleRate(sampleRate);
        bank->setSustain(false);
        bank->setPitchWheel(pitchWheel);
    }
    prepared = true;
}

void SoundFontEngine::selectInstrument()
{
    if (bank == nullptr || bank->instrumentCount() == 0)
        return;
    bank->selectInstrument(std::min(params.instrument, bank->instrumentCount() - 1));
}

void SoundFontEngine::setParams(const SoundFontParams& newParams)
{
    const int instrument = std::max(newParams.instrument, 0);
    const bool changed = instrument != params.instrument;
    params.instrument = instrument;
    params.velocityOn = newParams.velocityOn;
    params.echoMix = std::isnan(newParams.echoMix) ? 0.0f : std::clamp(newParams.echoMix, 0.0f, 1.0f);
    params.echoDelay = std::clamp(newParams.echoDelay, 0, SnesEcho::kMaxSteps);
    params.echoFeedback = std::isnan(newParams.echoFeedback) ? 0.0f : std::clamp(newParams.echoFeedback, 0.0f, 1.0f);
    echo.setDelay(params.echoDelay);

    if (changed && bank != nullptr)
    {
        bank->stopNotes();
        echo.reset();
        selectInstrument();
    }
}

void SoundFontEngine::setBank(SoundFontBank* newBank)
{
    if (newBank == bank)
        return;

    if (bank != nullptr)
        bank->stopNotes();
    echo.reset();
    bank = newBank;
    if (bank == nullptr)
        return;

    // It may have been played before, and may have been built for another rate.
    bank->stopNotes();
    if (prepared)
        bank->setSampleRate(sampleRate);
    selectInstrument();
    bank->setSustain(sustainOn);
    bank->setPitchWheel(pitchWheel);
}

void SoundFontEngine::noteOn(int note, int velocity)
{
    if (!prepared || note < 0 || note > 127)
        return;

    if (velocity <= 0)
    {
        noteOff(note);
        return;
    }

    if (bank == nullptr || bank->instrumentCount() == 0)
        return;

    bank->noteOn(note, params.velocityOn ? static_cast<float>(std::min(velocity, 127)) / 127.0f : 1.0f);
}

void SoundFontEngine::noteOff(int note)
{
    if (bank != nullptr && note >= 0 && note <= 127)
        bank->noteOff(note);
}

void SoundFontEngine::setPitchBend(double semitones)
{
    if (std::isnan(semitones))
        semitones = 0.0;
    const double bend = std::clamp(semitones, -kBendRange, kBendRange);
    pitchWheel = static_cast<int>(std::lround((bend + kBendRange) / (2.0 * kBendRange) * kWheelMax));
    if (bank != nullptr)
        bank->setPitchWheel(pitchWheel);
}

void SoundFontEngine::setSustain(bool on)
{
    sustainOn = on;
    if (bank != nullptr)
        bank->setSustain(on);
}

void SoundFontEngine::allNotesOff()
{
    // The pedal and the bend are left as they are, as in the other engines.
    if (bank != nullptr)
        bank->stopNotes();
    echo.reset();
}

void SoundFontEngine::render(float* out, int numSamples)
{
    if (numSamples <= 0)
        return;

    if (!prepared || bank == nullptr)
    {
        std::fill(out, out + numSamples, 0.0f);
        return;
    }

    bank->render(out, numSamples);
    if (kOutputGain != 1.0f)
        for (int i = 0; i < numSamples; ++i)
            out[i] *= kOutputGain;

    // Also when no note sounds: the echo rings on after the notes.
    echo.process(out, numSamples, params.echoMix, params.echoFeedback);
}

int SoundFontEngine::activeVoices() const { return bank != nullptr ? bank->activeVoices() : 0; }
} // namespace retrochip
