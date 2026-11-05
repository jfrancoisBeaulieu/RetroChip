// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "NesVoice.h"

#include "NesTables.h"
#include "nes/Dpcm.h"
#include "nes/N163Waves.h"

#include <algorithm>
#include <cmath>

namespace retrochip
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxLevel = 15.0;
constexpr int kMaxReleaseTicks = static_cast<int>(4.0 * nes::kTicksPerSecond); // a loop after the release ends here
constexpr int kFirstMidiNote = 12;                                              // FamiTracker's C-0
constexpr int kArpeggioSchemeMask = 0x3F;
constexpr int kArpeggioSchemeMax = 36;

double ticksFor(float milliseconds)
{
    return milliseconds * nes::kTicksPerSecond / 1000.0;
}

const FtiInstrument* instrumentFor(const NesParams& params, const NesUserSound* sound)
{
    if (!params.useInstrument || sound == nullptr)
        return nullptr;
    const auto& slot = sound->instruments[static_cast<std::size_t>(params.chip)];
    return slot.has_value() ? &*slot : nullptr;
}

// The kit's key for the note, when the 2A03's instrument has samples.
const FtiDpcmKey* kitKeyFor(int note, const FtiInstrument* instrument)
{
    if (instrument == nullptr)
        return nullptr;
    for (const auto& key : instrument->keys)
        if (key.note == note && key.sample >= 0 && key.sample < static_cast<int>(instrument->samples.size()))
            return &key;
    return nullptr;
}

bool hasKit(const FtiInstrument* instrument)
{
    return instrument != nullptr && !instrument->keys.empty();
}
} // namespace

NesVoice::Kind NesVoice::kindOf(const NesParams& params)
{
    if (params.chip == NesChip::Vrc6)
        return params.vrc6Saw ? Kind::Vrc6Saw : Kind::Vrc6Pulse;
    if (params.chip == NesChip::N163)
        return Kind::N163;
    switch (params.channel)
    {
        case NesChannel::Pulse: return Kind::Pulse;
        case NesChannel::Triangle: return Kind::Triangle;
        case NesChannel::Noise: return Kind::Noise;
        case NesChannel::Dpcm: return Kind::Dpcm;
    }
    return Kind::Pulse;
}

bool NesVoice::canPlay(int note, const NesParams& params, const NesUserSound* sound)
{
    if (kindOf(params) != Kind::Dpcm)
        return true;
    const auto* kit = instrumentFor(params, sound);
    if (hasKit(kit))
        return kitKeyFor(note, kit) != nullptr;
    return sound != nullptr && sound->dpcm.has_value() && !sound->dpcm->bytes.empty();
}

void NesVoice::start(int newNote, int newVelocity, double startPitch, std::uint64_t newAge, const NesParams& params, double bend,
                     const NesUserSound* sound)
{
    active = true;
    sustained = false;
    kind = kindOf(params);
    note = newNote;
    velocity = newVelocity;
    age = newAge;

    stage = Stage::Attack;
    envelope = 0.0;
    vibratoPhase = 0.0;

    const double glideTicks = ticksFor(params.glideMs);
    const bool glides = glideTicks > 1.0 && startPitch != note && kind != Kind::Noise && kind != Kind::Dpcm;
    pitch = glides ? startPitch : note;
    glideStep = glides ? (note - startPitch) / glideTicks : 0.0;

    sequenceStep = kind == Kind::Triangle ? nes::kTriangleFirstZeroStep : 0;
    vrc6Step = nes::kVrc6DutySteps - 1;
    sawStep = 0;
    wavePhase = 0.0;

    // The delta-modulation channel has no sequences: a 2A03 instrument gives it a kit, if it has samples.
    instrument = kind == Kind::Dpcm ? nullptr : instrumentFor(params, sound);
    for (auto& cursor : cursors)
        cursor.reset();
    arpeggioOffset = 0;
    fixedNote = -1;
    pitchOffset = 0.0;
    dutyValue = -1;
    releasedTicks = 0;
    levelSteps = 15;

    if (kind == Kind::Dpcm)
    {
        const auto* kit = instrumentFor(params, sound);
        const auto* key = hasKit(kit) ? kitKeyFor(note, kit) : nullptr;
        dpcmKey = key != nullptr;
        if (key != nullptr)
        {
            dpcmBytes = &kit->samples[static_cast<std::size_t>(key->sample)].bytes;
            dpcmRate = key->rate;
            dpcmLoop = key->loop;
            dpcmStart = key->delta >= 0 ? key->delta : dpcm::kMiddleLevel;
        }
        else
        {
            dpcmBytes = sound != nullptr && sound->dpcm.has_value() ? &sound->dpcm->bytes : nullptr;
            dpcmLoop = params.dpcmLoop;
            dpcmStart = dpcm::kMiddleLevel;
        }
        dpcmLevel = dpcmStart;
        dpcmBit = 0;
        dpcmEnded = dpcmBytes == nullptr || dpcmBytes->empty();
        if (dpcmEnded)
            active = false;
    }
    else if (instrument != nullptr)
    {
        stepInstrument(params);
    }
    else
    {
        advanceEnvelope(params);
    }

    selectWave(params);
    updatePeriod(params, bend);
    cyclesToNextStep = stepCycles;
}

void NesVoice::release()
{
    if (kind == Kind::Dpcm)
    {
        // A sample plays to its end, as the console's does, unless it loops: then the key stops it.
        if (dpcmLoop)
            active = false;
        return;
    }

    if (instrument != nullptr)
    {
        // As FamiTracker: a volume sequence with a release point goes on to its tail, anything else stops.
        const auto& volume = instrument->sequences[kFtiVolume];
        if (!volume.enabled || volume.release < 0)
        {
            active = false;
            return;
        }
        for (std::size_t i = 0; i < cursors.size(); ++i)
            cursors[i].release(instrument->sequences[i]);
    }

    stage = Stage::Release;
}

void NesVoice::tick(const NesParams& params, double bend)
{
    if (!active)
        return;

    if (instrument != nullptr)
        stepInstrument(params);
    else if (kind != Kind::Dpcm)
        advanceEnvelope(params);

    if (glideStep != 0.0)
    {
        pitch += glideStep;
        if ((glideStep > 0.0 && pitch >= note) || (glideStep < 0.0 && pitch <= note))
        {
            pitch = note;
            glideStep = 0.0;
        }
    }

    vibratoPhase += params.vibratoRateHz / nes::kTicksPerSecond;
    vibratoPhase -= std::floor(vibratoPhase);

    selectWave(params);
    updatePeriod(params, bend);
}

void NesVoice::advanceEnvelope(const NesParams& params)
{
    const double velocityScale = params.velocityOn ? velocity / 127.0 : 1.0;
    const double sustainLevel = std::clamp(static_cast<double>(params.sustain), 0.0, kMaxLevel);

    if (stage == Stage::Attack)
    {
        const double ticks = ticksFor(params.attackMs);
        envelope = ticks <= 1.0 ? kMaxLevel : std::min(envelope + kMaxLevel / ticks, kMaxLevel);
        if (envelope >= kMaxLevel)
            stage = Stage::Decay;
    }
    else if (stage == Stage::Decay)
    {
        const double ticks = ticksFor(params.decayMs);
        envelope = ticks <= 1.0 ? sustainLevel : envelope - (kMaxLevel - sustainLevel) / ticks;
        if (envelope <= sustainLevel)
            stage = Stage::Sustain;
    }
    else if (stage == Stage::Sustain)
    {
        envelope = sustainLevel;
    }
    else
    {
        // The triangle has no volume: it finishes its cycle in render() instead.
        if (kind != Kind::Triangle)
        {
            const double ticks = ticksFor(params.releaseMs);
            envelope = ticks <= 1.0 ? 0.0 : envelope - kMaxLevel / ticks;
            if (envelope <= 0.0)
            {
                envelope = 0.0;
                active = false;
            }
        }
    }

    // A zero-length attack reaches the decay stage within the first step.
    if (stage == Stage::Decay && ticksFor(params.decayMs) <= 1.0)
    {
        envelope = sustainLevel;
        stage = Stage::Sustain;
    }

    level = static_cast<int>(std::lround(std::clamp(envelope * velocityScale, 0.0, kMaxLevel)));
}

// One 60 Hz step of the instrument's sequences, which stand in for the envelope.
void NesVoice::stepInstrument(const NesParams& params)
{
    const auto& sequences = instrument->sequences;
    const bool sixtyFourSteps = kind == Kind::Vrc6Saw && sequences[kFtiVolume].setting == kVolume64Steps;
    levelSteps = sixtyFourSteps ? 63 : 15;

    int value = 0;
    const int volume = cursors[kFtiVolume].step(sequences[kFtiVolume], value) ? std::clamp(value, 0, levelSteps) : levelSteps;

    if (cursors[kFtiArpeggio].step(sequences[kFtiArpeggio], value))
    {
        switch (sequences[kFtiArpeggio].setting)
        {
            case kArpeggioFixed:
                fixedNote = kFirstMidiNote + value;
                break;
            case kArpeggioRelative:
                arpeggioOffset += value;
                fixedNote = -1;
                break;
            case kArpeggioScheme:
            {
                // The upper bits choose the effect column's x or y, which a plugin has none of.
                const int offset = value & kArpeggioSchemeMask;
                arpeggioOffset = offset > kArpeggioSchemeMax ? offset - (kArpeggioSchemeMask + 1) : offset;
                fixedNote = -1;
                break;
            }
            default:
                arpeggioOffset = value;
                fixedNote = -1;
                break;
        }
    }

    if (cursors[kFtiPitch].step(sequences[kFtiPitch], value))
        pitchOffset = sequences[kFtiPitch].setting == kPitchAbsolute ? value : pitchOffset + value;
    if (cursors[kFtiHiPitch].step(sequences[kFtiHiPitch], value))
        pitchOffset += 16.0 * value;
    if (cursors[kFtiDuty].step(sequences[kFtiDuty], value))
        dutyValue = value;

    // FamiTracker's mix of the sequence and the channel's volume, which never rounds a sound down to nothing.
    const double channelVolume = params.velocityOn ? std::max(1.0, std::round(velocity * kMaxLevel / 127.0)) : kMaxLevel;
    level = volume == 0 ? 0 : std::max(1, static_cast<int>(std::lround(volume * channelVolume / kMaxLevel)));

    if (sequences[kFtiVolume].enabled && cursors[kFtiVolume].finished() && volume == 0)
        active = false;
    if (stage == Stage::Release && ++releasedTicks > kMaxReleaseTicks)
        active = false;
}

void NesVoice::selectWave(const NesParams& params)
{
    if (kind != Kind::N163)
        return;
    if (instrument != nullptr && !instrument->waves.empty())
    {
        const int index = std::clamp(dutyValue, 0, static_cast<int>(instrument->waves.size()) - 1);
        wave = &instrument->waves[static_cast<std::size_t>(index)];
    }
    else
    {
        wave = &n163Waves()[static_cast<std::size_t>(std::clamp(params.n163Wave, 0, kN163WaveCount - 1))];
    }
    const auto length = static_cast<double>(wave->size());
    if (wavePhase >= length)
        wavePhase = std::fmod(wavePhase, length);
}

void NesVoice::updatePeriod(const NesParams& params, double bend)
{
    if (kind == Kind::Dpcm)
    {
        // A sample can only take the chip's 16 rates. The root note plays it at the fastest, where it was recorded.
        if (!dpcmKey)
            dpcmRate = dpcm::nearestRate(dpcm::rateHz(dpcm::kFastestRate) * std::pow(2.0, (note + bend - params.dpcmRootNote) / 12.0));
        stepCycles = dpcm::kRatePeriods[static_cast<std::size_t>(dpcmRate)];
        return;
    }

    const bool fixed = instrument != nullptr && fixedNote >= 0;
    if (kind == Kind::Noise)
    {
        stepCycles = nes::kNoisePeriods[static_cast<size_t>(nes::noiseIndexForNote(fixed ? fixedNote : note + arpeggioOffset))];
        return;
    }

    const double played = fixed ? fixedNote : pitch + arpeggioOffset;
    const double vibrato = params.vibratoDepthCents / 100.0 * std::sin(2.0 * kPi * vibratoPhase);
    const double frequency = 440.0 * std::pow(2.0, (played + bend + vibrato - 69.0) / 12.0);

    // Timers are not limited to 11 bits, so low notes stay in tune.
    if (kind == Kind::Pulse)
    {
        const double timer = std::max(std::round(nes::kCpuClock / (16.0 * frequency)) - 1.0 + pitchOffset, 0.0);
        pulseMuted = timer < nes::kMinAudiblePulseTimer;
        stepCycles = 2.0 * (timer + 1.0);
    }
    else if (kind == Kind::Triangle)
    {
        const double timer = std::max(std::round(nes::kCpuClock / (32.0 * frequency)) - 1.0 + pitchOffset,
                                      static_cast<double>(nes::kMinTriangleTimer));
        stepCycles = timer + 1.0;
    }
    else if (kind == Kind::Vrc6Pulse || kind == Kind::Vrc6Saw)
    {
        // The VRC6's timers have 12 bits. The saw takes 14 clocks a cycle, two per step.
        const double divider = kind == Kind::Vrc6Pulse ? 16.0 : 14.0;
        const double timer = std::clamp(std::round(nes::kCpuClock / (divider * frequency)) - 1.0 + pitchOffset, 0.0,
                                        static_cast<double>(nes::kVrc6MaxTimer));
        stepCycles = kind == Kind::Vrc6Pulse ? timer + 1.0 : 2.0 * (timer + 1.0);
    }
    else
    {
        // Each channel is updated once in every 15 cycles of each of the chip's channels. A pitch sequence counts in the
        // periods of a 2A03 pulse at the same note.
        const double pulsePeriod = nes::kCpuClock / (16.0 * frequency) + pitchOffset;
        const double bent = nes::kCpuClock / (16.0 * std::max(pulsePeriod, 1.0));
        const int channels = std::clamp(params.n163Channels, 1, nes::kN163MaxChannels);
        stepCycles = static_cast<double>(nes::kN163CyclesPerChannel * channels);
        waveStep = bent * static_cast<double>(wave->size()) * stepCycles / nes::kCpuClock;
    }
}

void NesVoice::advanceWaveform(const NesParams& params)
{
    switch (kind)
    {
        case Kind::Pulse:
            sequenceStep = (sequenceStep + 1) % 8;
            break;
        case Kind::Triangle:
            sequenceStep = (sequenceStep + 1) % 32;
            break;
        case Kind::Noise:
            lfsr = nes::stepLfsr(lfsr, dutyValue >= 0 ? (dutyValue & 1) != 0 : params.noiseShort);
            break;
        case Kind::Vrc6Pulse:
            vrc6Step = (vrc6Step + nes::kVrc6DutySteps - 1) % nes::kVrc6DutySteps; // it counts down
            break;
        case Kind::Vrc6Saw:
            sawStep = (sawStep + 1) % nes::kSawSteps;
            break;
        case Kind::N163:
        {
            const auto length = static_cast<double>(wave->size());
            wavePhase += waveStep;
            if (wavePhase >= length)
                wavePhase = std::fmod(wavePhase, length);
            break;
        }
        case Kind::Dpcm:
        {
            const auto bits = dpcmBytes->size() * 8;
            const bool up = (((*dpcmBytes)[dpcmBit / 8] >> (dpcmBit % 8)) & 1) != 0;
            dpcmLevel = dpcm::nextLevel(dpcmLevel, up);
            if (++dpcmBit >= bits)
            {
                dpcmBit = 0;
                dpcmEnded = !dpcmLoop;
            }
            break;
        }
    }
}

float NesVoice::currentAmp(const NesParams& params) const
{
    switch (kind)
    {
        case Kind::Pulse:
        {
            const int duty = dutyValue >= 0 ? (dutyValue & 3) : params.duty;
            if (pulseMuted || nes::kDutySequences[duty][sequenceStep] == 0)
                return 0.0f;
            return nes::kPulseAmp[static_cast<size_t>(level)];
        }
        case Kind::Triangle:
            // The triangle has no volume, but an instrument's silence stops it.
            if (instrument != nullptr && level == 0)
                return 0.0f;
            return nes::kTriangleAmp[nes::kTriangleSequence[sequenceStep]];
        case Kind::Noise:
            return (lfsr & 1) != 0 ? 0.0f : nes::kNoiseAmp[static_cast<size_t>(level)];
        case Kind::Vrc6Pulse:
        {
            const int duty = dutyValue >= 0 ? (dutyValue & 7) : std::clamp(params.vrc6Duty, 0, 7);
            return vrc6Step <= duty ? nes::kVrc6Unit * static_cast<float>(std::min(level, 15)) : 0.0f;
        }
        case Kind::Vrc6Saw:
        {
            // The volume is the saw's rate: what the accumulator adds every other clock, shown by its top 5 bits.
            const int rate = static_cast<int>(std::lround(level * nes::kSawMaxRate / levelSteps));
            return nes::kVrc6Unit * static_cast<float>((sawStep * rate) >> 3);
        }
        case Kind::N163:
            return nes::kN163Unit * static_cast<float>(((*wave)[static_cast<std::size_t>(wavePhase)] - 8) * level);
        case Kind::Dpcm:
            return nes::kDmcAmp[static_cast<size_t>(dpcmLevel)] - nes::kDmcAmp[static_cast<size_t>(dpcmStart)];
    }
    return 0.0f;
}

void NesVoice::emit(float amp, double samplePos, BandLimitedBuffer& buffer)
{
    if (amp != lastAmp)
    {
        buffer.addStep(samplePos, amp - lastAmp);
        lastAmp = amp;
    }
}

void NesVoice::render(int numSamples, BandLimitedBuffer& buffer, const NesParams& params, double cyclesPerSample)
{
    if (!active)
    {
        emit(0.0f, 0.0, buffer);
        return;
    }

    emit(currentAmp(params), 0.0, buffer);

    const double samplesPerStep = stepCycles / cyclesPerSample;
    double pos = cyclesToNextStep / cyclesPerSample;

    while (pos < numSamples)
    {
        advanceWaveform(params);
        emit(currentAmp(params), pos, buffer);

        if (stage == Stage::Release && kind == Kind::Triangle && instrument == nullptr
            && nes::kTriangleSequence[sequenceStep] == 0)
        {
            active = false;
            return;
        }
        if (kind == Kind::Dpcm && dpcmEnded)
        {
            active = false;
            emit(0.0f, pos, buffer);
            return;
        }

        pos += samplesPerStep;
    }

    cyclesToNextStep = (pos - numSamples) * cyclesPerSample;
}
} // namespace retrochip
