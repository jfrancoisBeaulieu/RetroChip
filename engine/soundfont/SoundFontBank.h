// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include <memory>
#include <string>

namespace retrochip
{
class SoundFontEngine;

namespace soundfont
{
// How many times the bank's TinySoundFont code has asked for memory, since the program began. Tests
// read it before and after audio-thread calls to prove they allocate nothing.
long long allocationCount();

// What the loader multiplies every zone's attenuation by. TinySoundFont keeps the format's centibels
// as a hundredth, so a zone written as 20 dB (200 cB) plays as 2 dB. Following the format would take 10;
// 4 follows what is believed to be common hardware and player behaviour, and the listening test decides.
constexpr float kAttenuationFix = 4.0f;
} // namespace soundfont

// One loaded .sf2 file, with its instruments listed. Construct and destroy it off the audio thread:
// loading allocates, and so does freeing. Instruments are numbered in TinySoundFont's order, which is
// by bank and then by program number, whatever order the file lists them in.
//
// A bank is played by one SoundFontEngine at a time. It is not copyable.
class SoundFontBank
{
public:
    // Null when the data is not a SoundFont this code can play: wrong format, damaged, or without
    // an instrument. The data is copied into the bank, so the caller may free it afterwards.
    static std::unique_ptr<SoundFontBank> fromMemory(const void* data, int size, double sampleRate);

    ~SoundFontBank();
    SoundFontBank(const SoundFontBank&) = delete;
    SoundFontBank& operator=(const SoundFontBank&) = delete;

    int instrumentCount() const;
    std::string instrumentName(int index) const; // empty outside the list
    int bankNumber(int index) const;             // -1 outside the list
    int programNumber(int index) const;          // -1 outside the list

    // The position of the instrument with this bank and program number, or -1.
    int indexOf(int bank, int program) const;

    // Stops every note. Not for the audio thread, though a bank that has no sound in it, as when it
    // is first handed to an engine, takes no time.
    void setSampleRate(double sampleRate);

private:
    friend class SoundFontEngine;
    struct Impl;
    explicit SoundFontBank(std::unique_ptr<Impl> impl);

    // What the engine plays with. Audio thread: none of these allocate or block.
    void selectInstrument(int index);
    void noteOn(int note, float velocity);
    void noteOff(int note);
    void setPitchWheel(int position); // 0 to 16383, 8192 centred; the range is 2 semitones each way
    void setSustain(bool on);
    void stopNotes();                 // at once, with nothing left to ring out
    int activeVoices() const;
    void render(float* out, int numSamples); // replaces the contents of out

    std::unique_ptr<Impl> impl;
};
} // namespace retrochip
