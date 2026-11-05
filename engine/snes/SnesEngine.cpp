// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "SnesEngine.h"

#include "Resampler.h"
#include "SnesEcho.h"
#include "SnesEnvelope.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace retrochip
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kTicksPerSecond = 60.0;
constexpr double kSamplesPerTick = SnesEngine::kChipRate / kTicksPerSecond;

constexpr int kFractionBits = 12;
constexpr std::int64_t kUnityStep = std::int64_t { 1 } << kFractionBits;
constexpr std::int64_t kBlockLength = std::int64_t { brr::kSamplesPerBlock } << kFractionBits;
constexpr int kInterpolationHistory = 3; // earlier samples the Gaussian window reads

// The step is deliberately not limited to the chip's 2 octaves, so notes stay in tune across the
// keyboard. This is past any note with its bend, and only keeps a stray bend from stalling the
// audio thread on blocks to decode.
constexpr double kMaxSemitonesFromRoot = 151.0;

// The user's sample, when it does not loop, ends on this fade, 5 ms at the chip's rate, so it does
// not click. It belongs here and not on the sample: a loop must not dip at its loop point.
constexpr std::int64_t kEndFadeLength = std::int64_t { 160 } << kFractionBits;

constexpr float kMinReleaseMs = 8.0f;
constexpr float kMaxReleaseMs = 2000.0f;

// A voice at full scale, ±0x4000, peaks at 0.25: close to a 2A03 pulse, as
// snes_output_level_comparable measures, and 8 voices stay within ±2.
constexpr double kOutputScale = 1.0 / 65536.0;

struct Voice
{
    bool active = false;
    bool sustained = false; // the player's key is up, held by the pedal
    int note = 0;
    int velocity = 127;
    std::uint64_t age = 0;

    const SnesSample* sample = nullptr;
    int block = 0;
    int lastBlock = 0; // where the sample ends
    int old = 0;
    int older = 0;
    std::array<std::int16_t, kInterpolationHistory + brr::kSamplesPerBlock> window {}; // the block, after the history
    std::int64_t counter = 0; // position in the block, with 12 bits of fraction
    std::int64_t step = 0;

    snes::SnesEnvelope envelope;

    double pitch = 0.0;
    double glideStep = 0.0;
    double vibratoPhase = 0.0;
};

int blockCount(const SnesSample& sample) { return static_cast<int>(sample.brr.size()) / brr::kBytesPerBlock; }

// The first block with the end flag, which is where nextBlock() stops, or else the last one.
int lastBlockOf(const SnesSample& sample)
{
    const int blocks = blockCount(sample);
    for (int block = 0; block < blocks; ++block)
        if (brr::hasEndFlag(sample.brr[static_cast<size_t>(block * brr::kBytesPerBlock)]))
            return block;
    return blocks - 1;
}
} // namespace

struct SnesEngine::Impl
{
    Resampler resampler;
    std::array<float, Resampler::kMaxChunk> scratch {};
    SnesEcho echo; // on the mixed voices at 32 kHz, before the resampler

    Impl() { echo.setDelay(params.echoDelay); }

    std::array<Voice, kNumVoices> voices;
    SnesParams params;
    snes::EnvelopeSettings envelope;

    const Bank* bank = nullptr;
    const SnesSample* userSample = nullptr;

    bool prepared = false;
    bool sustainOn = false;
    double bend = 0.0;
    int lastNote = -1;
    std::uint64_t nextAge = 0;
    double samplesToTick = kSamplesPerTick;

    const SnesSample* sampleForInstrument() const
    {
        const SnesSample* sample = nullptr;
        if (params.instrument == kUserInstrument)
            sample = userSample;
        else if (bank != nullptr && static_cast<size_t>(params.instrument) < bank->size())
            sample = (*bank)[static_cast<size_t>(params.instrument)].get();
        return sample != nullptr && blockCount(*sample) > 0 ? sample : nullptr;
    }

    void updateStep(Voice& voice) const
    {
        const int root = voice.sample == userSample ? params.rootNote : voice.sample->rootNote;
        const double vibrato = params.vibratoDepthCents / 100.0 * std::sin(2.0 * kPi * voice.vibratoPhase);
        const double semitones = std::clamp(voice.pitch + bend + vibrato - root, -kMaxSemitonesFromRoot, kMaxSemitonesFromRoot);
        voice.step = std::llround(static_cast<double>(kUnityStep) * std::pow(2.0, semitones / 12.0));
    }

    void decodeCurrentBlock(Voice& voice) const
    {
        // The new block's window starts with the last samples of the previous one.
        std::copy(voice.window.end() - kInterpolationHistory, voice.window.end(), voice.window.begin());
        const auto* data = voice.sample->brr.data() + voice.block * brr::kBytesPerBlock;
        brr::decodeBlock(data, voice.old, voice.older, voice.window.data() + kInterpolationHistory);
    }

    // Moves on from a finished block. False if the sample has ended.
    bool nextBlock(Voice& voice) const
    {
        const auto& sample = *voice.sample;
        const int blocks = blockCount(sample);
        const auto header = sample.brr[static_cast<size_t>(voice.block * brr::kBytesPerBlock)];
        int next = voice.block + 1;
        if (brr::hasEndFlag(header) || next >= blocks)
        {
            const bool validLoop = sample.loopBlock >= 0 && sample.loopBlock < blocks;
            if (&sample == userSample)
            {
                if (!params.loop)
                    return false;
                next = validLoop ? sample.loopBlock : 0; // snesLoop restarts a sample with no loop point
            }
            else
            {
                if (!validLoop)
                    return false;
                next = sample.loopBlock;
            }
        }
        voice.block = next; // old and older keep running, and the encoder uses filter 0 here
        decodeCurrentBlock(voice);
        return true;
    }

    void silence(Voice& voice)
    {
        voice.active = voice.sustained = false;
        voice.sample = nullptr;
        voice.envelope.stop();
    }

    void release(Voice& voice)
    {
        voice.sustained = false;
        voice.envelope.release();
    }

    int findVoiceFor(int note) const
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

    void start(Voice& voice, const SnesSample* sample, int note, int velocity)
    {
        const double startPitch = lastNote >= 0 ? lastNote : note;
        const double glideTicks = params.glideMs * kTicksPerSecond / 1000.0;
        const bool glides = glideTicks > 1.0 && startPitch != note;

        voice.active = true;
        voice.sustained = false;
        voice.note = note;
        voice.velocity = velocity;
        voice.age = nextAge++;
        voice.sample = sample;
        voice.block = 0;
        voice.lastBlock = lastBlockOf(*sample);
        voice.old = voice.older = 0;
        voice.window.fill(0);
        voice.counter = 0;
        voice.pitch = glides ? startPitch : note;
        voice.glideStep = glides ? (note - startPitch) / glideTicks : 0.0;
        voice.vibratoPhase = 0.0;
        voice.envelope.start();

        decodeCurrentBlock(voice);
        updateStep(voice);
        lastNote = note;
    }

    void tick()
    {
        for (auto& voice : voices)
        {
            if (!voice.active)
                continue;

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

            updateStep(voice);
        }
    }

    double renderVoice(Voice& voice)
    {
        voice.envelope.step(envelope);
        if (voice.envelope.finished())
        {
            silence(voice);
            return 0.0;
        }

        const auto position = static_cast<size_t>(voice.counter >> kFractionBits);
        const int index = static_cast<int>((voice.counter >> 4) & 0xFF);
        const int sample = brr::interpolate(voice.window.data() + position, index);

        double output = sample * (voice.envelope.level() / static_cast<double>(snes::kEnvelopeMax));
        if (params.velocityOn)
            output *= voice.velocity / 127.0;

        if (voice.sample == userSample && !params.loop)
        {
            // A short sample gives up at most a quarter of its length, so it still starts at full level.
            const std::int64_t length = (voice.lastBlock + 1) * kBlockLength;
            const std::int64_t fade = std::min(kEndFadeLength, length / 4);
            const std::int64_t left = (voice.lastBlock - voice.block + 1) * kBlockLength - voice.counter;
            if (left < fade)
                output *= static_cast<double>(left) / static_cast<double>(fade);
        }

        voice.counter += voice.step;
        while (voice.counter >= kBlockLength)
        {
            voice.counter -= kBlockLength;
            if (!nextBlock(voice))
            {
                silence(voice);
                break;
            }
        }
        return output;
    }

    // Runs the voices, tracking the 60 Hz step in chip samples so it doesn't depend on block size.
    void generate(float* out, int numSamples)
    {
        for (int n = 0; n < numSamples; ++n)
        {
            if (samplesToTick <= 0.0)
            {
                tick();
                samplesToTick += kSamplesPerTick;
            }
            samplesToTick -= 1.0;

            double sum = 0.0;
            for (auto& voice : voices)
                if (voice.active)
                    sum += renderVoice(voice);
            out[n] = static_cast<float>(sum * kOutputScale);
        }

        // Also when no voice sounds: the echo rings on after the notes.
        echo.process(out, numSamples, params.echoMix, params.echoFeedback);
    }
};

SnesEngine::SnesEngine() : impl(std::make_unique<Impl>()) {}

SnesEngine::~SnesEngine() = default;

void SnesEngine::prepare(double sampleRate)
{
    if (sampleRate <= 0.0)
        return;

    auto& e = *impl;
    e.voices.fill(Voice {});
    e.resampler.prepare(kChipRate, sampleRate);
    e.echo.reset();
    e.lastNote = -1;
    e.sustainOn = false;
    e.bend = 0.0;
    e.samplesToTick = kSamplesPerTick;
    e.prepared = true;
}

void SnesEngine::setParams(const SnesParams& newParams)
{
    auto& e = *impl;
    SnesParams clamped = newParams;
    clamped.instrument = std::clamp(clamped.instrument, 0, kUserInstrument);
    clamped.attack = std::clamp(clamped.attack, 0, 15);
    clamped.decay = std::clamp(clamped.decay, 0, 7);
    clamped.sustainLevel = std::clamp(clamped.sustainLevel, 0, 7);
    clamped.sustainRate = std::clamp(clamped.sustainRate, 0, 31);
    clamped.releaseMs = std::clamp(clamped.releaseMs, kMinReleaseMs, kMaxReleaseMs);
    clamped.rootNote = std::clamp(clamped.rootNote, 0, 127);
    clamped.echoMix = std::clamp(clamped.echoMix, 0.0f, 1.0f);
    clamped.echoDelay = std::clamp(clamped.echoDelay, 0, SnesEcho::kMaxSteps);
    clamped.echoFeedback = std::clamp(clamped.echoFeedback, 0.0f, 1.0f);

    if (clamped.instrument != e.params.instrument)
        allNotesOff();

    e.params = clamped;
    e.envelope.attack = clamped.attack;
    e.envelope.decay = clamped.decay;
    e.envelope.sustainLevel = clamped.sustainLevel;
    e.envelope.sustainRate = clamped.sustainRate;
    e.envelope.releaseStep = snes::envelopeReleaseStep(clamped.releaseMs);
    e.echo.setDelay(clamped.echoDelay);
}

void SnesEngine::setBank(const Bank* bank)
{
    auto& e = *impl;
    if (bank == e.bank)
        return;
    for (auto& voice : e.voices)
        if (voice.active && voice.sample != e.userSample)
            e.silence(voice);
    e.bank = bank;
}

void SnesEngine::setUserSample(const SnesSample* sample)
{
    auto& e = *impl;
    if (sample == e.userSample)
        return;
    for (auto& voice : e.voices)
        if (voice.active && voice.sample == e.userSample)
            e.silence(voice);
    e.userSample = sample;
}

void SnesEngine::noteOn(int note, int velocity)
{
    auto& e = *impl;
    if (!e.prepared || note < 0 || note > 127)
        return;

    if (velocity <= 0)
    {
        noteOff(note);
        return;
    }

    const auto* sample = e.sampleForInstrument();
    if (sample == nullptr)
        return;

    e.start(e.voices[static_cast<size_t>(e.findVoiceFor(note))], sample, note, std::min(velocity, 127));
}

void SnesEngine::noteOff(int note)
{
    auto& e = *impl;
    for (auto& voice : e.voices)
    {
        if (voice.active && voice.note == note && voice.envelope.phase() != snes::SnesEnvelope::Phase::Release)
        {
            if (e.sustainOn)
                voice.sustained = true;
            else
                e.release(voice);
        }
    }
}

void SnesEngine::setPitchBend(double semitones)
{
    impl->bend = semitones;
}

void SnesEngine::setSustain(bool on)
{
    auto& e = *impl;
    e.sustainOn = on;
    if (on)
        return;

    for (auto& voice : e.voices)
        if (voice.active && voice.sustained)
            e.release(voice);
}

void SnesEngine::allNotesOff()
{
    auto& e = *impl;
    for (auto& voice : e.voices)
        if (voice.active)
            e.silence(voice);

    // Nothing of what sounded may come back later, which also applies to the resampler's queue and
    // to the echo, which would ring on for seconds.
    e.resampler.reset();
    e.echo.reset();
    e.lastNote = -1;
    e.samplesToTick = kSamplesPerTick;
}

void SnesEngine::render(float* out, int numSamples)
{
    if (numSamples <= 0)
        return;

    auto& e = *impl;
    if (!e.prepared)
    {
        std::fill(out, out + numSamples, 0.0f);
        return;
    }

    while (numSamples > 0)
    {
        const int want = std::min(numSamples, Resampler::kMaxChunk);
        for (int needed = e.resampler.inputNeededFor(want); needed > 0;)
        {
            const int chunk = std::min(needed, Resampler::kMaxChunk);
            e.generate(e.scratch.data(), chunk);
            e.resampler.push(e.scratch.data(), chunk);
            needed -= chunk;
        }

        const int got = e.resampler.pull(out, want);
        if (got <= 0)
        {
            std::fill(out, out + numSamples, 0.0f); // can't happen with the input it was asked for
            return;
        }
        out += got;
        numSamples -= got;
    }
}

int SnesEngine::activeVoices() const
{
    return static_cast<int>(std::count_if(impl->voices.begin(), impl->voices.end(), [](const Voice& v) { return v.active; }));
}

bool SnesEngine::isNoteActive(int note) const
{
    return std::any_of(impl->voices.begin(), impl->voices.end(), [note](const Voice& v) { return v.active && v.note == note; });
}

int SnesEngine::latencySamples() const
{
    return impl->resampler.latencyOutputSamples();
}
} // namespace retrochip
