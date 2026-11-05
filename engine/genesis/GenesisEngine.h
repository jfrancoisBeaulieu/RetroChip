// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include "ConsoleEngine.h"
#include "FmPatches.h"
#include "Tfi.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace retrochip
{
struct GenesisParams
{
    int patch = 0;     // index into kFmPatches, or GenesisEngine::kUserPatch or kPcmPatch
    int algorithm = 7; // 0 to 7
    int feedback = 0;  // 0 to 7
    // Macros, -1 to +1. Positive brightness lowers the modulators' TL (more modulation), and
    // positive attack, decay and release lower the chip's rates (longer envelopes), like the
    // 2A03's time knobs. Attack can't take a rate below min(patch AR, 6), so no position
    // silences a note.
    // Release is capped at 2 s after key-off: 100 ms before that the chip's fastest release
    // finishes the note, so a longer setting fades until then and ends quickly, without a click.
    float brightness = 0.0f;
    float attack = 0.0f;
    float decay = 0.0f;
    float release = 0.0f;
    float vibratoRateHz = 5.0f;
    float vibratoDepthCents = 0.0f;
    float glideMs = 0.0f;
    bool velocityOn = true;
    int pcmRootNote = 60; // the key that plays the PCM sample as it was recorded
    bool pcmLoop = false; // see kPcmPatch
};

// One key of a kit.
struct PcmKitSample
{
    int note = 0;
    std::vector<std::uint8_t> pcm; // unsigned 8 bits, at GenesisEngine::kPcmRate
    std::string name;
};

// What the user has imported. Read-only once the engine has it.
struct GenesisUserSound
{
    std::optional<FmPatch> patch;
    std::array<std::uint8_t, kTfiSize> tfi {}; // the file the patch came from, for the plugin's state
    std::string patchName;

    std::vector<std::uint8_t> pcm; // unsigned 8 bits, at GenesisEngine::kPcmRate
    std::string pcmName;

    std::vector<PcmKitSample> kit; // at most one per note, none of them empty
};

// 8 voices on 2 YM2612s: channels 0 to 5 of the first chip, then 0 and 1 of the second. The
// chips run at their own rate and a Resampler brings the mono sum to the host's rate.
//
// The last channel of the second chip is its DAC, which nothing else uses. With the PCM patches the 8
// voices play the user's samples instead of FM: they are mixed and clipped to 8 bits for the one
// DAC, as a console's sound driver does, so a chord of loud notes distorts.
//
// Construct on a non-audio thread, which is where the chips allocate. prepare() may also run
// there. Everything else is safe on the audio thread: nothing allocates or blocks. Not copyable.
class GenesisEngine final : public ConsoleEngine
{
public:
    static constexpr int kNumVoices = 8;
    static constexpr int kClock = 7670453;                 // NTSC, Hz
    static constexpr double kChipRate = kClock / 144.0;    // about 53267 Hz

    // Values of GenesisParams::patch after kFmPatches. Without their sound, notes are ignored.
    static constexpr int kUserPatch = 12;
    // A sample without loop plays to its end whatever the key does. A looped one plays while the
    // key is down, then fades in 20 ms to 2 s, as the release macro goes from -1 to +1.
    static constexpr int kPcmPatch = 13;
    // Each key has its own sample, as a game's drums do. It plays once, at its recorded pitch, to its
    // end whatever the key does. Keys without a sample do nothing. The velocity still applies.
    static constexpr int kPcmKitPatch = 14;

    static constexpr int kPcmDivider = 4;                  // chip samples per DAC write
    static constexpr double kPcmRate = kChipRate / kPcmDivider; // about 13317 Hz

    GenesisEngine();
    ~GenesisEngine() override;

    void prepare(double sampleRate) override;
    void noteOn(int note, int velocity) override;
    void noteOff(int note) override;
    void setPitchBend(double semitones) override;
    void setSustain(bool on) override;
    void allNotesOff() override;
    void render(float* out, int numSamples) override;

    // Changing the patch stops sounding notes. Other changes reach them at the next 60 Hz step.
    void setParams(const GenesisParams& params);

    // The sound stays the caller's, and must outlive its use here. Null means none. Stops the
    // notes that were playing the previous one.
    void setUserSound(const GenesisUserSound* sound);

    int activeVoices() const;
    bool isNoteActive(int note) const;

    // Delay through the resampler, in output samples.
    int latencySamples() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace retrochip
