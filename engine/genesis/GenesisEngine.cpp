// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "GenesisEngine.h"

#include "FmPatches.h"
#include "Resampler.h"

#include "ymfm_opn.h"

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
constexpr double kSamplesPerTick = GenesisEngine::kChipRate / kTicksPerSecond;
constexpr int kReleaseTicks = static_cast<int>(2.0 * kTicksPerSecond); // a key-off voice counts as active for 2 s
constexpr int kFinishTicks = static_cast<int>(0.1 * kTicksPerSecond);  // the last stretch, where the chip finishes the note

constexpr int kChannelsPerChip = 6;
constexpr int kNumChips = 2;

// Register offsets of operators 1, 2, 3, 4: the chip's order is 1, 3, 2, 4.
constexpr std::array<int, 4> kOperatorOffset { 0, 8, 4, 12 };

constexpr int kMaxTl = 127;
constexpr int kMaxAdsRate = 31;
constexpr int kMaxReleaseRate = 15;
constexpr double kBrightnessTlRange = 40.0;
constexpr double kRateRange = 15.0;
constexpr int kMinMacroAttackRate = 6; // much lower and the note would take too long to be heard, or never start
constexpr double kVelocityTlRange = 24.0;
constexpr double kDbPerTl = 0.75;

// ym2612::generate() adds a DAC offset of +4 per silent channel, then scales the sum by
// 128 x 64 / (6 x 65), rounding toward zero. Removing it makes silence exactly zero.
constexpr std::int32_t kIdleOutputPerChip = (kChannelsPerChip * 4 * 128) * 64 / (6 * 65);

// ymfm averages the six channels, so a TL 0 sine peaks near 0.16 of its 16-bit range. That is
// already close to a 2A03 pulse, as genesis_output_level_comparable measures, so no gain is added.
constexpr double kOutputScale = 1.0 / 32768.0;

// Removes what the DAC offset and asymmetric feedback leave. Not the console's output filter:
// at this frequency it doesn't colour the sound.
constexpr double kDcBlockHz = 10.0;

constexpr int kDacSilence = 0x80; // the chip's 8-bit samples are unsigned
constexpr int kMaxDacSwing = 127;
constexpr double kMinPcmFadeSeconds = 0.02;
constexpr double kPcmFadeRatio = 100.0; // the longest fade, at the macro's top, over the shortest

// Operators that reach the output directly, as a bit mask with bit 0 for operator 1.
constexpr std::array<int, 8> kCarrierMask { 0b1000, 0b1000, 0b1000, 0b1000, 0b1010, 0b1110, 0b1110, 0b1111 };

// The engine uses no timer or interrupt, and writes without waiting for the chip's busy flag, so
// ymfm_interface's defaults are enough. There is one per chip because each chip registers itself
// with its interface.
class ChipInterface final : public ymfm::ymfm_interface
{
};

struct Voice
{
    bool active = false;
    bool released = false;      // the release is playing
    bool sustained = false;     // the player's key is up, held by the pedal
    bool keyOnPending = false;  // keyed off, waiting one chip sample so the chip sees the edge
    bool keyOffPending = false; // released before the key-on reached the chip
    bool finishing = false;     // near the end of the 2 s, on the chip's fastest release
    int note = 0;
    int velocity = 127;
    std::uint64_t age = 0;
    int releaseTicksLeft = 0;

    double pitch = 0.0;
    double glideStep = 0.0;
    double vibratoPhase = 0.0;
    int writtenPitch = -1; // the block and F-number on the chip

    const std::vector<std::uint8_t>* pcm = nullptr; // the sample it plays, which outlives it
    double pcmPosition = 0.0;
    double pcmStep = 1.0;     // sample frames per DAC write
    double pcmGain = 1.0;
    double pcmFade = 1.0;
    double pcmFadeStep = 0.0; // per DAC write, once a looped note is released
};
} // namespace

struct GenesisEngine::Impl
{
    std::array<ChipInterface, kNumChips> interfaces;
    std::array<ymfm::ym2612, kNumChips> chips { ymfm::ym2612 { interfaces[0] }, ymfm::ym2612 { interfaces[1] } };
    Resampler resampler;
    std::array<float, Resampler::kMaxChunk> scratch {};

    std::array<Voice, kNumVoices> voices;
    GenesisParams params;
    const GenesisUserSound* userSound = nullptr;

    bool prepared = false;
    bool sustainOn = false;
    bool macrosChanged = false;
    bool keysPending = false;
    double bend = 0.0;
    int lastNote = -1;
    std::uint64_t nextAge = 0;
    double samplesToTick = 0.0;
    int chipSamplesToDac = GenesisEngine::kPcmDivider;
    int writtenDac = kDacSilence;

    double dcCoefficient = 1.0 / (1.0 + 2.0 * kPi * kDcBlockHz / kChipRate);
    double dcInput = 0.0;
    double dcOutput = 0.0;

    bool playsPcm() const { return params.patch >= kPcmPatch; }
    bool playsKit() const { return params.patch == kPcmKitPatch; }
    bool loops() const { return params.pcmLoop && !playsKit(); }

    // What a note plays with a PCM patch, or null if nothing.
    const std::vector<std::uint8_t>* pcmFor(int note) const
    {
        if (userSound == nullptr)
            return nullptr;
        if (!playsKit())
            return userSound->pcm.empty() ? nullptr : &userSound->pcm;
        for (const auto& sample : userSound->kit)
            if (sample.note == note)
                return &sample.pcm;
        return nullptr;
    }

    bool hasSoundFor(int note) const
    {
        if (params.patch == kUserPatch)
            return userSound != nullptr && userSound->patch.has_value();
        return !playsPcm() || pcmFor(note) != nullptr;
    }

    // Only for a patch that has its sound. A voice can't outlive it: changing either stops the notes.
    const FmPatch& currentPatch() const
    {
        return params.patch == kUserPatch ? *userSound->patch : kFmPatches[static_cast<size_t>(params.patch)];
    }

    ymfm::ym2612& chipOf(int voice) { return chips[static_cast<size_t>(voice / kChannelsPerChip)]; }

    void writeGlobal(ymfm::ym2612& chip, int reg, int value)
    {
        chip.write_address(static_cast<std::uint8_t>(reg));
        chip.write_data(static_cast<std::uint8_t>(value));
    }

    // Channels 0 to 2 are on the lower port, 3 to 5 on the upper one, at the same registers.
    void writeChannel(int voice, int reg, int value)
    {
        auto& chip = chipOf(voice);
        const int channel = voice % kChannelsPerChip;
        const auto address = static_cast<std::uint8_t>(reg + channel % 3);
        if (channel < 3)
        {
            chip.write_address(address);
            chip.write_data(static_cast<std::uint8_t>(value));
        }
        else
        {
            chip.write_address_hi(address);
            chip.write_data_hi(static_cast<std::uint8_t>(value));
        }
    }

    void writeKey(int voice, bool on)
    {
        const int channel = voice % kChannelsPerChip;
        const int code = channel < 3 ? channel : channel + 1; // 3 is not a channel code
        writeGlobal(chipOf(voice), 0x28, (on ? 0xF0 : 0x00) | code);
    }

    // Positive macros mean longer, like the 2A03's time knobs, so they lower the rate.
    int rateWithMacro(int base, float macro, int maximum) const
    {
        return std::clamp(base - static_cast<int>(std::lround(macro * kRateRange)), 0, maximum);
    }

    // AR 0 never starts the note, so the macro can't take the rate below the floor. A patch that
    // is slower than the floor by itself keeps its own rate.
    int attackRateWithMacro(int base) const
    {
        return std::max(rateWithMacro(base, params.attack, kMaxAdsRate), std::min(base, kMinMacroAttackRate));
    }

    int levelFor(const Voice& voice, int op, int baseTl) const
    {
        const bool carrier = (kCarrierMask[static_cast<size_t>(params.algorithm)] >> op) & 1;
        double tl = baseTl;
        if (carrier)
        {
            if (params.velocityOn)
                tl += kVelocityTlRange * (127 - voice.velocity) / 126.0;
        }
        else
        {
            tl -= kBrightnessTlRange * params.brightness;
        }
        return std::clamp(static_cast<int>(std::lround(tl)), 0, kMaxTl);
    }

    // Everything the macros and the patch decide, for one voice.
    void writeSound(int index)
    {
        const auto& voice = voices[static_cast<size_t>(index)];
        const auto& patch = currentPatch();
        writeChannel(index, 0xB0, (params.feedback << 3) | params.algorithm);

        for (int op = 0; op < 4; ++op)
        {
            const auto& o = patch.ops[op];
            const int offset = kOperatorOffset[static_cast<size_t>(op)];
            writeChannel(index, 0x30 + offset, (o.dt << 4) | o.mul);
            writeChannel(index, 0x40 + offset, levelFor(voice, op, o.tl));
            writeChannel(index, 0x50 + offset, (o.rs << 6) | attackRateWithMacro(o.ar));
            writeChannel(index, 0x60 + offset, rateWithMacro(o.dr, params.decay, kMaxAdsRate));
            writeChannel(index, 0x70 + offset, o.sr);
            writeChannel(index, 0x80 + offset, (o.sl << 4) | (voice.finishing ? kMaxReleaseRate : rateWithMacro(o.rr, params.release, kMaxReleaseRate)));
            writeChannel(index, 0x90 + offset, o.ssg);
        }
    }

    void writePitch(int index)
    {
        auto& voice = voices[static_cast<size_t>(index)];
        const double vibrato = params.vibratoDepthCents / 100.0 * std::sin(2.0 * kPi * voice.vibratoPhase);
        const double frequency = 440.0 * std::pow(2.0, (voice.pitch + bend + vibrato - 69.0) / 12.0);

        // The smallest block that fits keeps the finest pitch resolution. Above about 6.6 kHz
        // (note 116) the chip has no more range, and the pitch stays at its limit.
        int block = 0;
        double fnum = 0.0;
        for (; block < 8; ++block)
        {
            fnum = std::round(frequency * 1048576.0 / (kChipRate * std::ldexp(1.0, block - 1)));
            if (fnum <= 2047.0)
                break;
        }
        if (block == 8)
        {
            block = 7;
            fnum = 2047.0;
        }

        const int written = (block << 11) | static_cast<int>(fnum);
        if (written == voice.writtenPitch)
            return;
        voice.writtenPitch = written;
        writeChannel(index, 0xA4, written >> 8); // latched until the write to 0xA0
        writeChannel(index, 0xA0, written & 0xFF);
    }

    // Lets the chip fade the note quickly, so a slow release ends without a click.
    void finish(int index)
    {
        voices[static_cast<size_t>(index)].finishing = true;
        const auto& patch = currentPatch();
        for (int op = 0; op < 4; ++op)
            writeChannel(index, 0x80 + kOperatorOffset[static_cast<size_t>(op)], (patch.ops[op].sl << 4) | kMaxReleaseRate);
    }

    // Keys off and drops the level, so the voice is silent however slow its release.
    void silence(int index)
    {
        auto& voice = voices[static_cast<size_t>(index)];
        writeKey(index, false);
        for (int op = 0; op < 4; ++op)
            writeChannel(index, 0x40 + kOperatorOffset[static_cast<size_t>(op)], kMaxTl);
        voice.active = voice.released = voice.sustained = voice.keyOnPending = voice.keyOffPending = voice.finishing = false;
    }

    double pcmFadeSeconds() const { return kMinPcmFadeSeconds * std::pow(kPcmFadeRatio, (params.release + 1.0) / 2.0); }

    void release(int index)
    {
        auto& voice = voices[static_cast<size_t>(index)];
        voice.released = true;
        voice.sustained = false;
        voice.releaseTicksLeft = kReleaseTicks;
        if (playsPcm())
        {
            // Without a loop the sample ends by itself, as it would on the console.
            if (loops())
                voice.pcmFadeStep = 1.0 / (pcmFadeSeconds() * kPcmRate);
            return;
        }
        if (voice.keyOnPending)
            voice.keyOffPending = true; // the note still sounds, however short
        else
            writeKey(index, false);
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

    void start(int index, int note, int velocity)
    {
        auto& voice = voices[static_cast<size_t>(index)];
        writeKey(index, false);

        const double startPitch = lastNote >= 0 ? lastNote : note;
        const double glideTicks = params.glideMs * kTicksPerSecond / 1000.0;
        const bool glides = glideTicks > 1.0 && startPitch != note;

        voice.active = true;
        voice.released = false;
        voice.sustained = false;
        voice.keyOnPending = !playsPcm();
        voice.keyOffPending = false;
        voice.finishing = false;
        voice.note = note;
        voice.velocity = velocity;
        voice.age = nextAge++;
        voice.pitch = glides ? startPitch : note;
        voice.glideStep = glides ? (note - startPitch) / glideTicks : 0.0;
        voice.vibratoPhase = 0.0;
        voice.writtenPitch = -1;
        lastNote = note;

        if (playsPcm())
        {
            // The same range in decibels as a carrier's level gets from the velocity.
            const double velocityDb = params.velocityOn ? -kVelocityTlRange * kDbPerTl * (127 - velocity) / 126.0 : 0.0;
            voice.pcm = pcmFor(note);
            voice.pcmPosition = 0.0;
            voice.pcmGain = std::pow(10.0, velocityDb / 20.0);
            voice.pcmFade = 1.0;
            voice.pcmFadeStep = 0.0;
            updatePcmStep(voice);
            return;
        }

        writeSound(index);
        writePitch(index);
        keysPending = true;
    }

    void updatePcmStep(Voice& voice)
    {
        if (playsKit())
        {
            voice.pcmStep = 1.0;
            return;
        }
        const double vibrato = params.vibratoDepthCents / 100.0 * std::sin(2.0 * kPi * voice.vibratoPhase);
        voice.pcmStep = std::pow(2.0, (voice.pitch + bend + vibrato - params.pcmRootNote) / 12.0);
    }

    void writeDac(int value)
    {
        if (value == writtenDac)
            return;
        writtenDac = value;
        writeGlobal(chips[1], 0x2A, value);
    }

    // One DAC write: the voices advance without interpolation, the way a console's driver reads samples.
    void stepPcm()
    {
        double mix = 0.0;
        for (int i = 0; i < kNumVoices; ++i)
        {
            auto& voice = voices[static_cast<size_t>(i)];
            if (!voice.active)
                continue;

            const auto& pcm = *voice.pcm;
            const auto frames = static_cast<double>(pcm.size());
            if (voice.pcmPosition >= frames)
            {
                // A loop that is turned on after the key was released doesn't bring the note back forever.
                if (!loops() || (voice.released && voice.pcmFadeStep <= 0.0))
                {
                    silence(i);
                    continue;
                }
                voice.pcmPosition = std::fmod(voice.pcmPosition, frames);
            }

            mix += (pcm[static_cast<size_t>(voice.pcmPosition)] - static_cast<double>(kDacSilence)) * voice.pcmGain * voice.pcmFade;
            voice.pcmPosition += voice.pcmStep;

            voice.pcmFade -= voice.pcmFadeStep;
            if (voice.pcmFade <= 0.0)
                silence(i);
        }
        writeDac(kDacSilence + std::clamp(static_cast<int>(std::lround(mix)), -kMaxDacSwing, kMaxDacSwing));
    }

    void tick()
    {
        for (int i = 0; i < kNumVoices; ++i)
        {
            auto& voice = voices[static_cast<size_t>(i)];
            if (!voice.active)
                continue;

            if (voice.released && !playsPcm())
            {
                if (--voice.releaseTicksLeft <= 0)
                {
                    silence(i); // a backstop: the chip has already finished the note
                    continue;
                }
                if (voice.releaseTicksLeft == kFinishTicks)
                    finish(i);
            }

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

            if (playsPcm())
            {
                updatePcmStep(voice);
                continue;
            }

            if (macrosChanged)
                writeSound(i);
            writePitch(i);
        }
        macrosChanged = false;
    }

    // Called after each chip sample while something waits, so each key change gets its own sample.
    void flushKeys()
    {
        keysPending = false;
        for (int i = 0; i < kNumVoices; ++i)
        {
            auto& voice = voices[static_cast<size_t>(i)];
            if (voice.keyOnPending)
            {
                voice.keyOnPending = false;
                writeKey(i, true);
                keysPending = keysPending || voice.keyOffPending;
            }
            else if (voice.keyOffPending)
            {
                voice.keyOffPending = false;
                writeKey(i, false);
            }
        }
    }

    // Runs the chips, tracking the 60 Hz step in chip samples so it doesn't depend on block size.
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

            if (--chipSamplesToDac <= 0)
            {
                chipSamplesToDac = kPcmDivider;
                if (playsPcm())
                    stepPcm();
            }

            ymfm::ym2612::output_data first, second;
            chips[0].generate(&first);
            chips[1].generate(&second);

            // The chips' left output, with the pan centred, is the mono output.
            const double input = (first.data[0] + second.data[0] - 2 * kIdleOutputPerChip) * kOutputScale;
            dcOutput = dcCoefficient * (dcOutput + input - dcInput);
            dcInput = input;
            out[n] = static_cast<float>(dcOutput);

            if (keysPending)
                flushKeys();
        }
    }
};

GenesisEngine::GenesisEngine() : impl(std::make_unique<Impl>()) {}

GenesisEngine::~GenesisEngine() = default;

void GenesisEngine::prepare(double sampleRate)
{
    if (sampleRate <= 0.0)
        return;

    auto& e = *impl;
    for (auto& chip : e.chips)
    {
        chip.reset();
        e.writeGlobal(chip, 0x22, 0x00); // no LFO
        e.writeGlobal(chip, 0x27, 0x00); // no timers, channel 3 in normal mode
        e.writeGlobal(chip, 0x2B, 0x00); // no DAC
    }
    for (int i = 0; i < kNumChips * kChannelsPerChip; ++i)
        e.writeChannel(i, 0xB4, 0xC0); // pan centre, on every channel so unused ones stay balanced

    // The second chip's last channel is no voice's, so its DAC can stay on. At rest it adds what the
    // silent channel it replaces would, which keeps kIdleOutputPerChip true.
    e.writeGlobal(e.chips[1], 0x2B, 0x80);
    e.writeGlobal(e.chips[1], 0x2A, kDacSilence);
    e.writtenDac = kDacSilence;
    e.chipSamplesToDac = kPcmDivider;

    e.voices.fill(Voice {});
    e.resampler.prepare(kChipRate, sampleRate);
    e.lastNote = -1;
    e.sustainOn = false;
    e.bend = 0.0;
    e.macrosChanged = false;
    e.keysPending = false;
    e.samplesToTick = kSamplesPerTick;
    e.dcInput = e.dcOutput = 0.0;
    e.prepared = true;
}

void GenesisEngine::setParams(const GenesisParams& newParams)
{
    auto& e = *impl;
    GenesisParams clamped = newParams;
    clamped.patch = std::clamp(clamped.patch, 0, kPcmKitPatch);
    clamped.pcmRootNote = std::clamp(clamped.pcmRootNote, 0, 127);
    clamped.algorithm = std::clamp(clamped.algorithm, 0, 7);
    clamped.feedback = std::clamp(clamped.feedback, 0, 7);
    clamped.brightness = std::clamp(clamped.brightness, -1.0f, 1.0f);
    clamped.attack = std::clamp(clamped.attack, -1.0f, 1.0f);
    clamped.decay = std::clamp(clamped.decay, -1.0f, 1.0f);
    clamped.release = std::clamp(clamped.release, -1.0f, 1.0f);

    const auto& old = e.params;
    const bool patchChanged = clamped.patch != old.patch;
    e.macrosChanged = e.macrosChanged || clamped.algorithm != old.algorithm || clamped.feedback != old.feedback
                      || clamped.brightness != old.brightness || clamped.attack != old.attack || clamped.decay != old.decay
                      || clamped.release != old.release || clamped.velocityOn != old.velocityOn;

    if (patchChanged)
        allNotesOff();
    e.params = clamped;
}

void GenesisEngine::setUserSound(const GenesisUserSound* sound)
{
    auto& e = *impl;
    if (sound == e.userSound)
        return;
    if (e.params.patch >= kUserPatch)
        allNotesOff();
    e.userSound = sound;
}

void GenesisEngine::noteOn(int note, int velocity)
{
    auto& e = *impl;
    if (!e.prepared || note < 0 || note > 127)
        return;

    if (velocity <= 0)
    {
        noteOff(note);
        return;
    }

    if (!e.hasSoundFor(note))
        return;

    e.start(e.findVoiceFor(note), note, std::min(velocity, 127));
}

void GenesisEngine::noteOff(int note)
{
    auto& e = *impl;
    for (int i = 0; i < kNumVoices; ++i)
    {
        auto& voice = e.voices[static_cast<size_t>(i)];
        if (voice.active && voice.note == note && !voice.released)
        {
            if (e.sustainOn)
                voice.sustained = true;
            else
                e.release(i);
        }
    }
}

void GenesisEngine::setPitchBend(double semitones)
{
    impl->bend = semitones;
}

void GenesisEngine::setSustain(bool on)
{
    auto& e = *impl;
    e.sustainOn = on;
    if (on)
        return;

    for (int i = 0; i < kNumVoices; ++i)
        if (e.voices[static_cast<size_t>(i)].active && e.voices[static_cast<size_t>(i)].sustained)
            e.release(i);
}

void GenesisEngine::allNotesOff()
{
    auto& e = *impl;
    for (int i = 0; i < kNumVoices; ++i)
        if (e.voices[static_cast<size_t>(i)].active)
            e.silence(i);
    e.keysPending = false;
    e.writeDac(kDacSilence);

    // Nothing of what sounded may come back later, which also applies to the resampler's queue and
    // to the DC blocker.
    e.resampler.reset();
    e.dcInput = e.dcOutput = 0.0;
    e.lastNote = -1;
}

void GenesisEngine::render(float* out, int numSamples)
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

int GenesisEngine::activeVoices() const
{
    return static_cast<int>(std::count_if(impl->voices.begin(), impl->voices.end(), [](const Voice& v) { return v.active; }));
}

bool GenesisEngine::isNoteActive(int note) const
{
    return std::any_of(impl->voices.begin(), impl->voices.end(), [note](const Voice& v) { return v.active && v.note == note; });
}

int GenesisEngine::latencySamples() const
{
    return impl->resampler.latencyOutputSamples();
}
} // namespace retrochip
