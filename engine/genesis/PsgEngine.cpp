// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "PsgEngine.h"

#include <algorithm>
#include <cmath>

namespace retrochip
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kTicksPerSecond = 60.0;
constexpr double kSamplesPerTick = PsgEngine::kChipRate / kTicksPerSecond;

constexpr int kSilentAttenuation = 15;
constexpr double kDbPerStep = 2.0;
constexpr int kVelocitySteps = 8; // close to the 18 dB a YM2612 carrier gets from the velocity

// A full square has about the loudness of a YM2612 sine at TL 0, as psg_level_is_close_to_the_other_chips measures.
constexpr double kFullLevel = 0.125;

// Removes what periodic noise, high one step in 16, would leave as an offset.
constexpr double kDcBlockHz = 10.0;

constexpr int kFixedNoiseDivider = 16; // doubled by each of the chip's lower rates
constexpr double kPeriodicSteps = 16.0;

double stepsFor(float milliseconds)
{
    return static_cast<double>(milliseconds) * kTicksPerSecond / 1000.0;
}
} // namespace

PsgEngine::PsgEngine()
{
    for (int i = 0; i < kSilentAttenuation; ++i)
        levels[static_cast<size_t>(i)] = kFullLevel * std::pow(10.0, -kDbPerStep * i / 20.0);
    levels[kSilentAttenuation] = 0.0;
}

void PsgEngine::prepare(double sampleRate)
{
    if (sampleRate <= 0.0)
        return;

    voices.fill(Voice {});
    resampler.prepare(kChipRate, sampleRate);
    sustainOn = false;
    bend = 0.0;
    lastNote = -1;
    samplesToTick = kSamplesPerTick;
    dcCoefficient = 1.0 / (1.0 + 2.0 * kPi * kDcBlockHz / kChipRate);
    dcInput = dcOutput = 0.0;
    prepared = true;
}

void PsgEngine::setParams(const PsgParams& newParams)
{
    if (newParams.noise != params.noise)
        allNotesOff();

    params = newParams;
    params.noiseRate = std::clamp(params.noiseRate, 0, kNoiseFollowsNote);
    params.sustain = std::clamp(params.sustain, 0, kSilentAttenuation);
}

void PsgEngine::updatePitch(Voice& voice)
{
    if (params.noise && params.noiseRate != kNoiseFollowsNote)
    {
        voice.divider = kFixedNoiseDivider << params.noiseRate;
        return;
    }

    const double vibrato = params.vibratoDepthCents / 100.0 * std::sin(2.0 * kPi * voice.vibratoPhase);
    double frequency = 440.0 * std::pow(2.0, (voice.pitch + bend + vibrato - 69.0) / 12.0);

    // The register has 16 steps in periodic mode, so its rate is 16 times the note. White noise goes by the same
    // rule, which puts the middle of the keyboard around the chip's own rates.
    if (params.noise)
        frequency *= kPeriodicSteps;

    voice.divider = static_cast<int>(std::clamp(std::lround(kClock / (32.0 * frequency)), 1L, static_cast<long>(kMaxDivider)));
}

void PsgEngine::updateLevel(Voice& voice)
{
    const int velocitySteps = params.velocityOn ? static_cast<int>(std::lround(kVelocitySteps * (127 - voice.velocity) / 126.0)) : 0;
    const int attenuation = kSilentAttenuation - static_cast<int>(std::lround(voice.envelope * kSilentAttenuation)) + velocitySteps;
    voice.level = levels[static_cast<size_t>(std::min(attenuation, kSilentAttenuation))];
}

void PsgEngine::stepEnvelope(Voice& voice)
{
    const double held = params.sustain / static_cast<double>(kSilentAttenuation);

    switch (voice.stage)
    {
        case Stage::attack:
        {
            const double steps = stepsFor(params.attackMs);
            voice.envelope = steps > 1.0 ? voice.envelope + 1.0 / steps : 1.0;
            if (voice.envelope >= 1.0)
            {
                voice.envelope = 1.0;
                voice.stage = Stage::decay;
            }
            break;
        }
        case Stage::decay:
        {
            const double steps = stepsFor(params.decayMs);
            voice.envelope = steps > 1.0 ? voice.envelope - 1.0 / steps : held;
            if (voice.envelope <= held)
            {
                voice.envelope = held;
                voice.stage = Stage::sustain;
            }
            break;
        }
        case Stage::sustain:
            voice.envelope = held;
            break;
        case Stage::release:
            voice.envelope -= voice.releaseStep;
            break;
    }

    // Nothing is left to hear, and the voice can serve another note.
    if (voice.envelope <= 0.0 && (voice.stage == Stage::release || voice.stage == Stage::sustain))
        voice.active = false;
}

void PsgEngine::tick()
{
    for (auto& voice : voices)
    {
        if (!voice.active)
            continue;

        stepEnvelope(voice);

        if (voice.glideStep != 0.0)
        {
            voice.pitch += voice.glideStep;
            if ((voice.glideStep > 0.0 && voice.pitch >= voice.note) || (voice.glideStep < 0.0 && voice.pitch <= voice.note))
            {
                voice.pitch = voice.note;
                voice.glideStep = 0.0;
            }
        }

        voice.vibratoPhase += params.vibratoRateHz / kTicksPerSecond;
        voice.vibratoPhase -= std::floor(voice.vibratoPhase);

        updatePitch(voice);
        updateLevel(voice);
    }
}

// Tracks the 60 Hz step in chip samples, so it doesn't depend on block size.
void PsgEngine::generate(float* out, int numSamples)
{
    for (int n = 0; n < numSamples; ++n)
    {
        if (samplesToTick <= 0.0)
        {
            tick();
            samplesToTick += kSamplesPerTick;
        }
        samplesToTick -= 1.0;

        double input = 0.0;
        for (auto& voice : voices)
        {
            if (!voice.active)
                continue;

            if (--voice.counter <= 0)
            {
                voice.counter = voice.divider;
                voice.high = !voice.high;
                if (params.noise && voice.high)
                    voice.noise = nextNoise(voice.noise, !params.periodic);
            }

            const bool up = params.noise ? (voice.noise & 1) != 0 : voice.high;
            input += up ? voice.level : -voice.level;
        }

        dcOutput = dcCoefficient * (dcOutput + input - dcInput);
        dcInput = input;
        out[n] = static_cast<float>(dcOutput);
    }
}

int PsgEngine::findVoiceFor(int note) const
{
    for (int i = 0; i < kNumVoices; ++i)
        if (voices[static_cast<size_t>(i)].active && voices[static_cast<size_t>(i)].note == note)
            return i;
    for (int i = 0; i < kNumVoices; ++i)
        if (!voices[static_cast<size_t>(i)].active)
            return i;

    int oldest = 0;
    for (int i = 1; i < kNumVoices; ++i)
        if (voices[static_cast<size_t>(i)].age < voices[static_cast<size_t>(oldest)].age)
            oldest = i;
    return oldest;
}

void PsgEngine::noteOn(int note, int velocity)
{
    if (!prepared || note < 0 || note > 127)
        return;

    if (velocity <= 0)
    {
        noteOff(note);
        return;
    }

    auto& voice = voices[static_cast<size_t>(findVoiceFor(note))];
    const double startPitch = lastNote >= 0 ? lastNote : note;
    const double glideTicks = stepsFor(params.glideMs);
    const bool glides = glideTicks > 1.0 && startPitch != note;
    const bool attacks = stepsFor(params.attackMs) > 1.0;

    voice.active = true;
    voice.sustained = false;
    voice.note = note;
    voice.velocity = std::min(velocity, 127);
    voice.age = nextAge++;
    voice.stage = attacks ? Stage::attack : Stage::decay;
    voice.envelope = attacks ? 0.0 : 1.0;
    voice.pitch = glides ? startPitch : note;
    voice.glideStep = glides ? (note - startPitch) / glideTicks : 0.0;
    voice.vibratoPhase = 0.0;
    voice.high = false;
    voice.noise = 0x8000;

    updatePitch(voice);
    updateLevel(voice);
    voice.counter = voice.divider;
    lastNote = note;
}

void PsgEngine::release(Voice& voice)
{
    voice.sustained = false;
    voice.stage = Stage::release;
    const double steps = stepsFor(params.releaseMs);
    if (steps <= 0.0)
    {
        voice.active = false;
        return;
    }
    voice.releaseStep = 1.0 / std::max(steps, 1.0);
}

void PsgEngine::noteOff(int note)
{
    for (auto& voice : voices)
    {
        if (voice.active && voice.note == note && voice.stage != Stage::release)
        {
            if (sustainOn)
                voice.sustained = true;
            else
                release(voice);
        }
    }
}

void PsgEngine::setPitchBend(double semitones)
{
    bend = semitones;
}

void PsgEngine::setSustain(bool on)
{
    sustainOn = on;
    if (on)
        return;

    for (auto& voice : voices)
        if (voice.active && voice.sustained)
            release(voice);
}

void PsgEngine::allNotesOff()
{
    // Nothing of what sounded may come back later, which also applies to the resampler's queue and to the DC blocker.
    for (auto& voice : voices)
        voice = Voice {};
    resampler.reset();
    dcInput = dcOutput = 0.0;
    lastNote = -1;
}

void PsgEngine::render(float* out, int numSamples)
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
        const int want = std::min(numSamples, Resampler::kMaxChunk);
        for (int needed = resampler.inputNeededFor(want); needed > 0;)
        {
            const int chunk = std::min(needed, Resampler::kMaxChunk);
            generate(scratch.data(), chunk);
            resampler.push(scratch.data(), chunk);
            needed -= chunk;
        }

        const int got = resampler.pull(out, want);
        if (got <= 0)
        {
            std::fill(out, out + numSamples, 0.0f); // can't happen with the input it was asked for
            return;
        }
        out += got;
        numSamples -= got;
    }
}

int PsgEngine::activeVoices() const
{
    return static_cast<int>(std::count_if(voices.begin(), voices.end(), [](const Voice& v) { return v.active; }));
}

bool PsgEngine::isNoteActive(int note) const
{
    return std::any_of(voices.begin(), voices.end(), [note](const Voice& v) { return v.active && v.note == note; });
}

int PsgEngine::latencySamples() const
{
    return resampler.latencyOutputSamples();
}
} // namespace retrochip
