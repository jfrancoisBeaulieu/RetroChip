// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include "ConsoleEngine.h"
#include "snes/SnesEcho.h"

namespace retrochip
{
class SoundFontBank;

struct SoundFontParams
{
    int instrument = 0; // index into the bank; one past its end plays the last instrument
    bool velocityOn = true;
    float echoMix = 0.0f;      // 0 to 1
    int echoDelay = 4;         // steps of 16 ms, 0 to 15; 0 is off
    float echoFeedback = 0.3f; // 0 to 1
};

// Plays one instrument of a SoundFontBank on TinySoundFont's channel 0, mono, with at most 32 voices,
// and the S-DSP's echo (SnesEcho) on the sum of the voices.
// When all 32 are busy and none is releasing, TinySoundFont ignores a new note, which is accepted.
//
// The bank is not owned: the caller builds and frees it off the audio thread. Everything here is
// safe on the audio thread: nothing allocates or blocks. prepare() is not an audio-thread call, and
// asks the bank to follow the new sample rate and sizes the echo for it. Not copyable.
class SoundFontEngine final : public ConsoleEngine
{
public:
    SoundFontEngine();
    ~SoundFontEngine() override;
    SoundFontEngine(const SoundFontEngine&) = delete;
    SoundFontEngine& operator=(const SoundFontEngine&) = delete;

    void prepare(double sampleRate) override;
    void noteOn(int note, int velocity) override;
    void noteOff(int note) override;
    void setPitchBend(double semitones) override;
    void setSustain(bool on) override;
    void allNotesOff() override;
    void render(float* out, int numSamples) override;

    // Changing the instrument stops sounding notes and empties the echo. The echo settings are
    // held in range; a new delay never plays old data.
    void setParams(const SoundFontParams& params);

    // Stops the notes of the old bank and of the new one and empties the echo, and keeps the pointer. The caller may free
    // the old bank once the call has returned. The new bank must stay valid until the next call, or
    // until the engine is destroyed. Null means no sound. The pedal and the bend carry over to it.
    void setBank(SoundFontBank* bank);

    int activeVoices() const;

    // The engine adds no delay.
    int latencySamples() const { return 0; }

private:
    void selectInstrument();

    SoundFontBank* bank = nullptr;
    SoundFontParams params;
    SnesEcho echo; // on the voices, after the bank and its output gain
    bool prepared = false;
    bool sustainOn = false;
    int pitchWheel = 8192;
    double sampleRate = 0.0;
};
} // namespace retrochip
