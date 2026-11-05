// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include "BandLimitedBuffer.h"
#include "ConsoleEngine.h"
#include "NesVoice.h"

#include <array>
#include <cstdint>

namespace retrochip
{
// The 2A03's channels, and the VRC6's and Namco 163's, which Famicom cartridges add: one kind of channel at a time,
// on 8 voices.
class NesEngine final : public ConsoleEngine
{
public:
    static constexpr int kNumVoices = 8;

    void prepare(double sampleRate) override;
    void noteOn(int note, int velocity) override;
    void noteOff(int note) override;
    void setPitchBend(double semitones) override;
    void setSustain(bool on) override;
    void allNotesOff() override;
    void render(float* out, int numSamples) override;

    // Changing the channel, the chip or the instrument stops sounding notes, since they can't change waveform mid-note.
    void setParams(const NesParams& newParams);

    // The sound stays the caller's and must outlive its use here. Null means none. Stops sounding notes.
    void setUserSound(const NesUserSound* sound);

    int activeVoices() const;
    bool isNoteActive(int note) const;

private:
    NesVoice* findVoiceFor(int note);

    std::array<NesVoice, kNumVoices> voices;
    BandLimitedBuffer buffer;
    NesParams params;
    const NesUserSound* userSound = nullptr;

    bool prepared = false;
    bool sustainOn = false;
    double bend = 0.0;
    int lastNote = -1;
    std::uint64_t nextAge = 0;

    double cyclesPerSample = 1.0;
    double samplesPerTick = 1.0;
    double samplesToTick = 0.0;

    double highPassCoefficient = 0.0;
    double lowPassCoefficient = 1.0;
    double highPassInput = 0.0;
    double highPassOutput = 0.0;
    double lowPassOutput = 0.0;
};
} // namespace retrochip
