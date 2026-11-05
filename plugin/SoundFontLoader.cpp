// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "SoundFontLoader.h"

#include <new>
#include <utility>

namespace retrochip
{
juce::String decodeInstrumentName(const std::string& bytes)
{
    if (juce::CharPointer_UTF8::isValidString(bytes.c_str(), static_cast<int>(bytes.size())))
        return juce::String::fromUTF8(bytes.c_str(), static_cast<int>(bytes.size()));

    juce::String latin1;
    latin1.preallocateBytes(bytes.size() * 2);
    for (const char c : bytes)
        latin1 += static_cast<juce::juce_wchar>(static_cast<unsigned char>(c)); // its codes are Unicode's first 256
    return latin1;
}

SoundFontLoader::SoundFontLoader(std::function<void(Result)> finished)
    : juce::Thread("RetroChip SoundFont loader"), onFinished(std::move(finished))
{
}

SoundFontLoader::~SoundFontLoader() { stop(); }

juce::uint64 SoundFontLoader::request(const juce::File& file, double sampleRate, int bank, int program)
{
    juce::uint64 id = 0;
    {
        const juce::ScopedLock scope(lock);
        id = ++latestId;
        pending = Job { id, file, sampleRate, bank, program };
    }
    if (!isThreadRunning())
        startThread();
    notify();
    return id;
}

void SoundFontLoader::cancel()
{
    const juce::ScopedLock scope(lock);
    ++latestId;
    pending.reset();
}

void SoundFontLoader::stop()
{
    cancel();
    stopThread(-1); // a read cannot be interrupted, and forcing a thread down could leave a lock held
}

void SoundFontLoader::run()
{
    while (!threadShouldExit())
    {
        std::optional<Job> job;
        {
            const juce::ScopedLock scope(lock);
            job.swap(pending);
        }
        if (!job.has_value())
        {
            wait(-1);
            continue;
        }

        auto result = load(*job);

        bool latest = false;
        {
            const juce::ScopedLock scope(lock);
            latest = job->id == latestId;
        }
        if (latest)
            onFinished(std::move(result));
        // Otherwise the bank goes here, on this thread: nothing else has seen it.
    }
}

SoundFontLoader::Result SoundFontLoader::load(const Job& job) const
{
    Result result;
    result.id = job.id;
    result.file = job.file;

    if (!job.file.existsAsFile())
    {
        result.failure = Failure::missing;
        return result;
    }
    if (job.file.getSize() > kMaxFileBytes)
    {
        result.failure = Failure::tooLarge;
        return result;
    }

    try
    {
        juce::MemoryBlock data;
        if (!job.file.loadFileAsData(data))
        {
            result.failure = Failure::unreadable;
            return result;
        }
        if (static_cast<juce::int64>(data.getSize()) > kMaxFileBytes) // it grew after the size was read
        {
            result.failure = Failure::tooLarge;
            return result;
        }

        // Closing a project must not wait for a parse nobody will use. stop() has already cancelled this request,
        // so the failure is never delivered.
        if (threadShouldExit())
        {
            result.failure = Failure::unreadable;
            return result;
        }

        result.bank = SoundFontBank::fromMemory(data.getData(), static_cast<int>(data.getSize()), job.sampleRate);
        if (result.bank == nullptr)
        {
            result.failure = Failure::unreadable;
            return result;
        }

        const int count = result.bank->instrumentCount();
        result.instruments.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i)
            result.instruments.push_back({ decodeInstrumentName(result.bank->instrumentName(i)), result.bank->bankNumber(i), result.bank->programNumber(i) });

        const int index = result.bank->indexOf(job.bank, job.program);
        result.instrumentFound = index >= 0;
        result.instrument = juce::jmax(index, 0);
    }
    catch (const std::bad_alloc&)
    {
        // A large file needs a lot of memory at once, for itself and the bank's copy. The host goes on without it.
        result = {};
        result.id = job.id;
        result.file = job.file;
        result.failure = Failure::unreadable;
    }
    return result;
}
} // namespace retrochip
