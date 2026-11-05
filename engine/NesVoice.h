// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include "BandLimitedBuffer.h"
#include "nes/Fti.h"
#include "nes/NesSequence.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace retrochip
{
enum class NesChannel
{
    Pulse,
    Triangle,
    Noise,
    Dpcm
};

// The chip the NES console plays: its own, or one of the Famicom cartridges' that add channels.
enum class NesChip
{
    Ricoh2A03,
    Vrc6,
    N163
};

struct NesParams
{
    NesChannel channel = NesChannel::Pulse;
    int duty = 2; // 0 to 3: 12.5, 25, 50, 75 %
    bool noiseShort = false;
    float attackMs = 0.0f;
    float decayMs = 0.0f;
    float sustain = 15.0f; // 0 to 15
    float releaseMs = 50.0f;
    float vibratoRateHz = 5.0f;
    float vibratoDepthCents = 0.0f;
    float glideMs = 0.0f;
    bool velocityOn = true;

    NesChip chip = NesChip::Ricoh2A03;
    bool vrc6Saw = false;
    int vrc6Duty = 7;     // 0 to 7: 1 to 8 steps high out of 16
    int n163Wave = 0;     // into n163Waves()
    int n163Channels = 1; // 1 to 8: the more the chip runs, the slower each one updates
    int dpcmRootNote = 60;
    bool dpcmLoop = false;
    bool useInstrument = false; // the chip's imported instrument instead of the envelope
};

// What the user has imported for the NES. Read-only once the engine has it.
struct NesUserSound
{
    std::array<std::optional<FtiInstrument>, 3> instruments; // by NesChip
    std::optional<DpcmSample> dpcm;                          // a WAV, at the fastest rate
};

class NesVoice
{
public:
    enum class Kind
    {
        Pulse,
        Triangle,
        Noise,
        Dpcm,
        Vrc6Pulse,
        Vrc6Saw,
        N163
    };

    static Kind kindOf(const NesParams& params);

    // Whether the note has something to play: the delta-modulation channel needs a sample for it.
    static bool canPlay(int note, const NesParams& params, const NesUserSound* sound);

    // startPitch is the note the glide comes from, or note itself for no glide. The sound must outlive the note.
    void start(int note, int velocity, double startPitch, std::uint64_t age, const NesParams& params, double bend,
               const NesUserSound* sound = nullptr);
    void release();
    void stop() { active = false; }

    // Advances envelope or instrument, vibrato, glide, and period by one 60 Hz step.
    void tick(const NesParams& params, double bend);

    void render(int numSamples, BandLimitedBuffer& buffer, const NesParams& params, double cyclesPerSample);

    bool isActive() const { return active; }
    bool isReleased() const { return stage == Stage::Release; }
    int getNote() const { return note; }
    std::uint64_t getAge() const { return age; }

    bool sustained = false; // key is up, held by the pedal

private:
    enum class Stage
    {
        Attack,
        Decay,
        Sustain,
        Release
    };

    void advanceEnvelope(const NesParams& params);
    void stepInstrument(const NesParams& params);
    void updatePeriod(const NesParams& params, double bend);
    void selectWave(const NesParams& params);
    void advanceWaveform(const NesParams& params);
    float currentAmp(const NesParams& params) const;
    void emit(float amp, double samplePos, BandLimitedBuffer& buffer);

    bool active = false;
    bool pulseMuted = false;
    Kind kind = Kind::Pulse;
    int note = 0;
    int velocity = 127;
    std::uint64_t age = 0;

    Stage stage = Stage::Attack;
    double envelope = 0.0;
    int level = 0;
    int levelSteps = 15; // 63 for a VRC6 saw on a 64-step volume sequence

    double pitch = 0.0;
    double glideStep = 0.0;
    double vibratoPhase = 0.0;

    // The instrument's sequences, while it plays one.
    const FtiInstrument* instrument = nullptr;
    std::array<SequenceCursor, kFtiSequenceCount> cursors;
    int arpeggioOffset = 0;
    int fixedNote = -1;
    double pitchOffset = 0.0; // added to the period
    int dutyValue = -1;       // the duty or wave sequence's, or -1 for the parameters'
    int releasedTicks = 0;

    // The delta-modulation channel.
    const std::vector<std::uint8_t>* dpcmBytes = nullptr;
    std::size_t dpcmBit = 0;
    int dpcmLevel = 64;
    int dpcmStart = 64;
    int dpcmRate = 15;
    bool dpcmKey = false; // a kit's key, at its own rate
    bool dpcmLoop = false;
    bool dpcmEnded = false;

    // The cartridge chips.
    int vrc6Step = 15;
    int sawStep = 0;
    const std::vector<std::uint8_t>* wave = nullptr;
    double wavePhase = 0.0;
    double waveStep = 0.0;

    int sequenceStep = 0;
    std::uint16_t lfsr = 1;
    double stepCycles = 1.0;
    double cyclesToNextStep = 0.0;
    float lastAmp = 0.0f;
};
} // namespace retrochip
