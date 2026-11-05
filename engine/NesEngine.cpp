// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "NesEngine.h"

#include "NesTables.h"
#include "nes/N163Waves.h"

#include <algorithm>
#include <cmath>

namespace retrochip
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kHighPassHz = 90.0;
constexpr double kLowPassHz = 14000.0;
} // namespace

void NesEngine::prepare(double sampleRate)
{
    if (sampleRate <= 0.0)
        return;

    for (auto& voice : voices)
        voice = NesVoice {};
    buffer.clear();
    n163Waves(); // built here rather than on the audio thread

    cyclesPerSample = nes::kCpuClock / sampleRate;
    samplesPerTick = sampleRate / nes::kTicksPerSecond;
    samplesToTick = samplesPerTick;

    highPassCoefficient = 1.0 / (1.0 + 2.0 * kPi * kHighPassHz / sampleRate);
    const double lowPassHz = std::min(kLowPassHz, 0.45 * sampleRate);
    lowPassCoefficient = 1.0 - std::exp(-2.0 * kPi * lowPassHz / sampleRate);
    highPassInput = highPassOutput = lowPassOutput = 0.0;

    sustainOn = false;
    bend = 0.0;
    lastNote = -1;

    prepared = true;
}

void NesEngine::setParams(const NesParams& newParams)
{
    if (NesVoice::kindOf(newParams) != NesVoice::kindOf(params) || newParams.useInstrument != params.useInstrument)
        for (auto& voice : voices)
            voice.stop();

    params = newParams;
    params.duty = std::clamp(params.duty, 0, 3);
    params.vrc6Duty = std::clamp(params.vrc6Duty, 0, 7);
    params.n163Wave = std::clamp(params.n163Wave, 0, kN163WaveCount - 1);
    params.n163Channels = std::clamp(params.n163Channels, 1, nes::kN163MaxChannels);
    params.dpcmRootNote = std::clamp(params.dpcmRootNote, 0, 127);
}

void NesEngine::setUserSound(const NesUserSound* sound)
{
    if (sound == userSound)
        return;
    for (auto& voice : voices)
        voice.stop();
    userSound = sound;
}

NesVoice* NesEngine::findVoiceFor(int note)
{
    NesVoice* oldest = nullptr;
    for (auto& voice : voices)
    {
        if (voice.isActive() && voice.getNote() == note)
            return &voice;
        if (!voice.isActive())
            oldest = &voice;
    }
    if (oldest != nullptr)
        return oldest;

    oldest = voices.data();
    for (auto& voice : voices)
        if (voice.getAge() < oldest->getAge())
            oldest = &voice;
    return oldest;
}

void NesEngine::noteOn(int note, int velocity)
{
    if (!prepared || note < 0 || note > 127)
        return;

    if (velocity <= 0)
    {
        noteOff(note);
        return;
    }

    if (!NesVoice::canPlay(note, params, userSound))
        return;

    const double startPitch = lastNote >= 0 ? lastNote : note;
    findVoiceFor(note)->start(note, std::min(velocity, 127), startPitch, nextAge++, params, bend, userSound);
    lastNote = note;
}

void NesEngine::noteOff(int note)
{
    for (auto& voice : voices)
    {
        if (voice.isActive() && voice.getNote() == note && !voice.isReleased())
        {
            if (sustainOn)
                voice.sustained = true;
            else
                voice.release();
        }
    }
}

void NesEngine::setPitchBend(double semitones)
{
    bend = semitones;
}

void NesEngine::setSustain(bool on)
{
    sustainOn = on;
    if (on)
        return;

    for (auto& voice : voices)
    {
        if (voice.isActive() && voice.sustained)
        {
            voice.sustained = false;
            voice.release();
        }
    }
}

void NesEngine::allNotesOff()
{
    // Nothing of what sounded may come back later, which also applies to what the buffer and the filters
    // still hold, and to the level each voice would take back from the emptied buffer.
    for (auto& voice : voices)
        voice = NesVoice {};
    buffer.clear();
    highPassInput = highPassOutput = lowPassOutput = 0.0;
    lastNote = -1;
}

void NesEngine::render(float* out, int numSamples)
{
    if (numSamples <= 0)
        return;

    if (!prepared)
    {
        std::fill(out, out + numSamples, 0.0f);
        return;
    }

    while (numSamples > 0)
    {
        if (samplesToTick <= 0.0)
        {
            for (auto& voice : voices)
                voice.tick(params, bend);
            samplesToTick += samplesPerTick;
        }

        const int chunk = std::min({ numSamples,
                                     BandLimitedBuffer::kMaxChunk,
                                     static_cast<int>(std::ceil(samplesToTick)) });

        for (auto& voice : voices)
            voice.render(chunk, buffer, params, cyclesPerSample);

        buffer.read(out, chunk);

        for (int i = 0; i < chunk; ++i)
        {
            const double input = out[i];
            highPassOutput = highPassCoefficient * (highPassOutput + input - highPassInput);
            highPassInput = input;
            lowPassOutput += lowPassCoefficient * (highPassOutput - lowPassOutput);
            out[i] = static_cast<float>(lowPassOutput);
        }

        samplesToTick -= chunk;
        out += chunk;
        numSamples -= chunk;
    }
}

int NesEngine::activeVoices() const
{
    return static_cast<int>(std::count_if(voices.begin(), voices.end(), [](const NesVoice& v) { return v.isActive(); }));
}

bool NesEngine::isNoteActive(int note) const
{
    return std::any_of(voices.begin(), voices.end(), [note](const NesVoice& v) { return v.isActive() && v.getNote() == note; });
}
} // namespace retrochip
