// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include "Brr.h"
#include "ConsoleEngine.h"

#include <memory>
#include <vector>

namespace retrochip
{
struct SnesParams
{
    int instrument = 0;     // index into the bank, or SnesEngine::kUserInstrument
    int attack = 15;        // 0 to 15, the chip's rates
    int decay = 7;          // 0 to 7
    int sustainLevel = 7;   // 0 to 7, for 1/8 to 8/8
    int sustainRate = 0;    // 0 (holds) to 31
    float releaseMs = 8.0f; // 8 to 2000, linear
    float echoMix = 0.0f;
    int echoDelay = 4;
    float echoFeedback = 0.3f;
    float vibratoRateHz = 5.0f;
    float vibratoDepthCents = 0.0f;
    float glideMs = 0.0f;
    bool velocityOn = true;
    int rootNote = 60; // for the user's sample; a bank sample has its own
    bool loop = false; // for the user's sample; a bank sample has its loopBlock
};

// 8 sample voices at the chip's 32 kHz, each with BRR decoding, Gaussian interpolation and an
// ADSR envelope, then the echo on their mono sum. A Resampler brings that to the host's rate.
// The user's sample fades over its last 5 ms when it does not loop.
//
// Construct on a non-audio thread, which is where the resampler allocates. prepare() may also run
// there. Everything else is safe on the audio thread: nothing allocates or blocks. Not copyable.
class SnesEngine final : public ConsoleEngine
{
public:
    static constexpr int kNumVoices = 8;
    static constexpr double kChipRate = 32000.0;
    static constexpr int kUserInstrument = 12;

    using Bank = std::vector<std::shared_ptr<const SnesSample>>;

    SnesEngine();
    ~SnesEngine() override;

    void prepare(double sampleRate) override;
    void noteOn(int note, int velocity) override;
    void noteOff(int note) override;
    void setPitchBend(double semitones) override;
    void setSustain(bool on) override;
    void allNotesOff() override;
    void render(float* out, int numSamples) override;

    // Changing the instrument stops sounding notes. The envelope follows other changes at once;
    // pitch follows them at the next 60 Hz step.
    void setParams(const SnesParams& params);

    // Both keep the pointer, and stop voices that use the old samples, so the caller may free
    // those once the call has returned. The new samples must stay valid and unchanged until the
    // next call, or until the engine is destroyed. Null means no samples.
    void setBank(const Bank* bank);
    void setUserSample(const SnesSample* sample);

    int activeVoices() const;
    bool isNoteActive(int note) const;

    // Delay through the resampler, in output samples.
    int latencySamples() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace retrochip
