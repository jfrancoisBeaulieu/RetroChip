// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include "soundfont/SoundFontBank.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace retrochip
{
struct SoundFontInstrument
{
    juce::String name;
    int bank = 0;
    int program = 0;
};

// The SF2 format does not say how its names are encoded: UTF-8 if the bytes are valid as UTF-8, else Latin-1.
juce::String decodeInstrumentName(const std::string& bytes);

// Reads .sf2 files and builds their banks on a thread of its own, one at a time. The thread starts with the first
// request, and it waits for the next one for as long as the loader lives.
class SoundFontLoader final : private juce::Thread
{
public:
    static constexpr juce::int64 kMaxFileBytes = 1024LL * 1024 * 1024;

    enum class Failure
    {
        none,
        missing,
        unreadable, // cannot be read, or is not a SoundFont this code can play
        tooLarge
    };

    struct Result
    {
        juce::uint64 id = 0; // what request() returned
        juce::File file;
        Failure failure = Failure::none;

        // With no failure. Nothing else has seen the bank yet, and the list is read from it here so that nobody
        // needs to touch it again off the audio thread.
        std::unique_ptr<SoundFontBank> bank;
        std::vector<SoundFontInstrument> instruments;
        int instrument = 0;           // the index of the requested bank and program, else the first
        bool instrumentFound = false; // whether that bank and program exist
    };

    // Called on the loader's thread, without any of its locks, so it may call request() and cancel().
    explicit SoundFontLoader(std::function<void(Result)> onFinished);
    ~SoundFontLoader() override;

    // Asks for a file at a sample rate, with the instrument to look up. A request not yet started when the next
    // arrives is dropped, and one under way is not delivered, so the last request wins. Returns its id.
    juce::uint64 request(const juce::File& file, double sampleRate, int bank, int program);

    // Drops the last request as a newer one would, with nothing to replace it.
    void cancel();

    // Waits for a read under way to end, and skips the building of the bank that would follow it. No result is
    // delivered after this returns. A message loop is not needed.
    void stop();

private:
    struct Job
    {
        juce::uint64 id;
        juce::File file;
        double sampleRate;
        int bank;
        int program;
    };

    void run() override;
    Result load(const Job& job) const;

    std::function<void(Result)> onFinished;
    juce::CriticalSection lock;
    std::optional<Job> pending;
    juce::uint64 latestId = 0;

    JUCE_DECLARE_NON_COPYABLE(SoundFontLoader)
};
} // namespace retrochip
