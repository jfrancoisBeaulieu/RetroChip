// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "NesEngine.h"
#include "NesTables.h"
#include "Resampler.h"
#include "genesis/FmPatches.h"
#include "genesis/GenesisEngine.h"
#include "genesis/PsgEngine.h"
#include "genesis/Tfi.h"
#include "nes/Dpcm.h"
#include "nes/Fti.h"
#include "nes/N163Waves.h"
#include "nes/NesSequence.h"
#include "snes/Brr.h"
#include "snes/BrrEncoder.h"
#include "snes/GaussTable.h"
#include "snes/SnesBank.h"
#include "snes/SnesEcho.h"
#include "snes/SnesEngine.h"
#include "Sf2Builder.h"
#include "snes/SnesEnvelope.h"
#include "soundfont/SoundFontBank.h"
#include "soundfont/SoundFontEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Counts every C++ allocation in the test program, so a test can prove the audio path makes none.
static std::atomic<long long> allocationCount { 0 };

void* operator new(std::size_t size)
{
    ++allocationCount;
    if (void* p = std::malloc(size > 0 ? size : 1))
        return p;
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace retrochip;

namespace
{
struct TestCase
{
    const char* name;
    std::function<void()> run;
};

std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

int failures = 0;

struct Registrar
{
    Registrar(const char* name, std::function<void()> run) { registry().push_back({ name, std::move(run) }); }
};

#define TEST(name)                                       \
    static void name();                                  \
    static const Registrar registrar_##name(#name, name); \
    static void name()

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                      \
        }                                                                    \
    } while (false)

#define CHECK_NEAR(value, expected, tolerance)                                                     \
    do                                                                                             \
    {                                                                                              \
        const double v = (value);                                                                  \
        const double e = (expected);                                                               \
        if (std::abs(v - e) > (tolerance))                                                         \
        {                                                                                          \
            std::printf("  FAILED %s:%d: %s = %f, expected %f\n", __FILE__, __LINE__, #value, v, e); \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

constexpr double kPi = 3.14159265358979323846;

std::vector<float> render(ConsoleEngine& engine, int numSamples)
{
    std::vector<float> out(static_cast<size_t>(numSamples));
    engine.render(out.data(), numSamples);
    return out;
}

double rms(const std::vector<float>& x, size_t from = 0)
{
    double sum = 0.0;
    for (size_t i = from; i < x.size(); ++i)
        sum += static_cast<double>(x[i]) * x[i];
    return std::sqrt(sum / static_cast<double>(x.size() - from));
}

double peak(const std::vector<float>& x)
{
    double p = 0.0;
    for (auto s : x)
        p = std::max(p, std::abs(static_cast<double>(s)));
    return p;
}

bool allFinite(const std::vector<float>& x)
{
    for (auto s : x)
        if (!std::isfinite(s))
            return false;
    return true;
}

// Measures frequency from interpolated rising zero crossings. The signal must drop
// well below zero between crossings, so ringing around square edges isn't counted.
double measureFrequency(const std::vector<float>& x, double sampleRate, size_t from = 0)
{
    const auto threshold = static_cast<float>(-0.25 * peak(x));
    double first = -1.0, last = -1.0;
    int count = 0;
    bool armed = false;
    for (size_t i = std::max<size_t>(from, 1); i < x.size(); ++i)
    {
        if (x[i] < threshold)
            armed = true;

        if (armed && x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            armed = false;
            const double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0.0)
                first = t;
            last = t;
            ++count;
        }
    }
    return count < 2 ? 0.0 : (count - 1) * sampleRate / (last - first);
}

// Hann-windowed single-frequency magnitude.
double magnitudeAt(const std::vector<float>& x, double frequency, double sampleRate)
{
    double re = 0.0, im = 0.0;
    const auto n = x.size();
    for (size_t i = 0; i < n; ++i)
    {
        const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
        const double phase = 2.0 * kPi * frequency * static_cast<double>(i) / sampleRate;
        re += window * x[i] * std::cos(phase);
        im += window * x[i] * std::sin(phase);
    }
    return std::sqrt(re * re + im * im) / static_cast<double>(n);
}

double decibels(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-12)); }

double pulseFrequency(int note)
{
    const double f = 440.0 * std::pow(2.0, (note - 69) / 12.0);
    const double period = std::round(nes::kCpuClock / (16.0 * f));
    return nes::kCpuClock / (16.0 * period);
}

NesEngine makeEngine(NesChannel channel, double sampleRate = 48000.0)
{
    NesEngine engine;
    NesParams params;
    params.channel = channel;
    params.releaseMs = 0.0f;
    engine.setParams(params);
    engine.prepare(sampleRate);
    return engine;
}

// Pushes the whole signal in chunks, pulling after each one, and returns everything produced.
std::vector<float> resample(Resampler& resampler, const std::vector<float>& in, int chunk = 1000)
{
    std::vector<float> out;
    std::vector<float> block(4096);
    for (size_t pos = 0; pos < in.size(); pos += static_cast<size_t>(chunk))
    {
        const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(chunk), in.size() - pos));
        resampler.push(in.data() + pos, n);
        for (;;)
        {
            const int got = resampler.pull(block.data(), static_cast<int>(block.size()));
            out.insert(out.end(), block.begin(), block.begin() + got);
            if (got < static_cast<int>(block.size()))
                break;
        }
    }
    return out;
}

std::vector<float> sine(double frequency, double sampleRate, int numSamples, double amplitude = 1.0)
{
    std::vector<float> x(static_cast<size_t>(numSamples));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / sampleRate));
    return x;
}

double noteFrequency(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }

double centsBetween(double frequency, double expected) { return 1200.0 * std::log2(std::max(frequency, 1e-6) / expected); }

// Its chips refer to their interfaces, so the engine can't be copied or returned by value.
std::unique_ptr<GenesisEngine> makeGenesis(const GenesisParams& params, double sampleRate = 48000.0)
{
    auto engine = std::make_unique<GenesisEngine>();
    engine->setParams(params);
    engine->prepare(sampleRate);
    return engine;
}

GenesisParams paramsForPatch(int patch)
{
    GenesisParams params;
    params.patch = patch;
    params.algorithm = kFmPatches[static_cast<size_t>(patch)].algorithm;
    params.feedback = kFmPatches[static_cast<size_t>(patch)].feedback;
    return params;
}

// Patch numbers the tests rely on, in the order Task 4's names use.
constexpr int kPatchPureTone = 0, kPatchEPiano = 3, kPatchBell = 4, kPatchLead = 6, kPatchOrgan = 7, kPatchPad = 8;

// Energy of harmonics 2 to 12 relative to the fundamental, in dB.
double harmonicDb(const std::vector<float>& x, double frequency, double sampleRate)
{
    const double fundamental = magnitudeAt(x, frequency, sampleRate);
    double energy = 0.0;
    for (int harmonic = 2; harmonic <= 12; ++harmonic)
    {
        const double m = magnitudeAt(x, frequency * harmonic, sampleRate);
        energy += m * m;
    }
    return 10.0 * std::log10(std::max(energy, 1e-24) / std::max(fundamental * fundamental, 1e-24));
}

// Harmonic energy of a held A3, skipping the attack.
double heldHarmonicDb(const GenesisParams& params)
{
    auto engine = makeGenesis(params);
    engine->noteOn(57, 127);
    render(*engine, 24000);
    return harmonicDb(render(*engine, 24000), noteFrequency(57), 48000.0);
}

// The loudest stretch of 50 ms.
double loudestWindowRms(const std::vector<float>& x, size_t window = 2400)
{
    double loudest = 0.0;
    for (size_t pos = 0; pos + window <= x.size(); pos += window / 2)
    {
        double sum = 0.0;
        for (size_t i = pos; i < pos + window; ++i)
            sum += static_cast<double>(x[i]) * x[i];
        loudest = std::max(loudest, std::sqrt(sum / static_cast<double>(window)));
    }
    return loudest;
}

// Seconds until every later 10 ms window stays below the level; -1 if the signal never gets there.
double secondsUntilBelow(const std::vector<float>& x, double level, double sampleRate)
{
    const auto window = static_cast<size_t>(sampleRate / 100.0);
    double lastLoud = -1.0;
    size_t pos = 0;
    for (; pos + window <= x.size(); pos += window)
    {
        double sum = 0.0;
        for (size_t i = pos; i < pos + window; ++i)
            sum += static_cast<double>(x[i]) * x[i];
        if (std::sqrt(sum / static_cast<double>(window)) >= level)
            lastLoud = static_cast<double>(pos + window) / sampleRate;
    }
    return lastLoud >= static_cast<double>(pos) / sampleRate - 0.011 ? -1.0 : std::max(lastLoud, 0.0);
}

struct DecodedBlock
{
    std::array<std::int16_t, brr::kSamplesPerBlock> out {};
    int old = 0;
    int older = 0;
};

std::array<int, brr::kSamplesPerBlock> constantNibbles(int nibble)
{
    std::array<int, brr::kSamplesPerBlock> nibbles {};
    nibbles.fill(nibble);
    return nibbles;
}

void appendBrrBlock(std::vector<std::uint8_t>& out, int shift, int filter, int flags, const std::array<int, brr::kSamplesPerBlock>& nibbles)
{
    out.push_back(static_cast<std::uint8_t>((shift << 4) | (filter << 2) | flags));
    for (size_t i = 0; i < nibbles.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(((nibbles[i] & 0x0F) << 4) | (nibbles[i + 1] & 0x0F)));
}

// Decodes one hand-written block, with the nibbles given first and zeros after them.
DecodedBlock decodeBrr(int shift, int filter, std::initializer_list<int> firstNibbles, int old = 0, int older = 0, int flags = 0)
{
    std::array<int, brr::kSamplesPerBlock> nibbles {};
    std::copy(firstNibbles.begin(), firstNibbles.end(), nibbles.begin());
    std::vector<std::uint8_t> block;
    appendBrrBlock(block, shift, filter, flags, nibbles);
    DecodedBlock decoded;
    decoded.old = old;
    decoded.older = older;
    brr::decodeBlock(block.data(), decoded.old, decoded.older, decoded.out.data());
    return decoded;
}

// A minimal stand-in for the encoder of a later task: filter 0, shift 12, so 15-bit samples are
// rounded to the nearest 16 levels. Flags the last block as the end, and as a loop if it has one.
SnesSample brrFromSamples(const std::vector<int>& samples, int loopBlock)
{
    SnesSample sample;
    sample.loopBlock = loopBlock;
    sample.rootNote = 60;
    const size_t blocks = samples.size() / brr::kSamplesPerBlock;
    for (size_t block = 0; block < blocks; ++block)
    {
        std::array<int, brr::kSamplesPerBlock> nibbles {};
        for (size_t i = 0; i < nibbles.size(); ++i)
            nibbles[i] = std::clamp(static_cast<int>(std::lround(samples[block * brr::kSamplesPerBlock + i] / 2048.0)), -8, 7);
        const int flags = block + 1 < blocks ? 0 : (loopBlock >= 0 ? 3 : 1);
        appendBrrBlock(sample.brr, 12, 0, flags, nibbles);
    }
    return sample;
}

// A sine with a period of 100 samples. A block holds 16, so the loop takes 4 periods to end on a
// block boundary. At the root note it plays at 320 Hz, from the chip's 32 kHz.
SnesSample loopedSine()
{
    std::vector<int> samples(400);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int>(std::lround(14000.0 * std::sin(2.0 * kPi * static_cast<double>(i) / 100.0)));
    return brrFromSamples(samples, 0);
}

double loopedSineFrequency(int note) { return 320.0 * std::pow(2.0, (note - 60) / 12.0); }

SnesSample constantSample(int nibble, int blocks)
{
    return brrFromSamples(std::vector<int>(static_cast<size_t>(blocks * brr::kSamplesPerBlock), nibble * 2048), -1);
}

SnesEngine::Bank bankOf(const SnesSample& sample) { return { std::make_shared<const SnesSample>(sample) }; }

// The engine keeps a pointer to the bank, which the caller must keep alive. Without a bank the
// engine gets none.
std::unique_ptr<SnesEngine> makeSnes(const SnesEngine::Bank& bank, const SnesParams& params = {}, double sampleRate = 48000.0)
{
    auto engine = std::make_unique<SnesEngine>();
    if (!bank.empty())
        engine->setBank(&bank);
    engine->setParams(params);
    engine->prepare(sampleRate);
    return engine;
}
} // namespace

TEST(pulse_pitch_matches_rounded_hardware_period)
{
    auto engine = makeEngine(NesChannel::Pulse);
    engine.noteOn(69, 127);
    const auto out = render(engine, 96000);
    CHECK_NEAR(measureFrequency(out, 48000.0, 4800), nes::kCpuClock / (16.0 * 254.0), 0.5);
}

TEST(triangle_pitch_matches_rounded_hardware_period)
{
    auto engine = makeEngine(NesChannel::Triangle);
    engine.noteOn(69, 127);
    const auto out = render(engine, 96000);
    CHECK_NEAR(measureFrequency(out, 48000.0, 4800), nes::kCpuClock / (32.0 * 127.0), 0.5);
}

TEST(duty_settings_produce_expected_harmonic_nulls)
{
    const auto harmonicDb = [](int duty, int harmonic)
    {
        NesEngine engine;
        NesParams params;
        params.duty = duty;
        engine.setParams(params);
        engine.prepare(48000.0);
        engine.noteOn(69, 127);
        const auto out = render(engine, 96000);
        const double f = pulseFrequency(69);
        return decibels(magnitudeAt(out, f * harmonic, 48000.0) / magnitudeAt(out, f, 48000.0));
    };

    CHECK(harmonicDb(2, 2) < -40.0); // 50 %: no even harmonics
    CHECK(harmonicDb(1, 2) > -20.0);
    CHECK(harmonicDb(1, 4) < -40.0); // 25 %: every 4th missing
    CHECK(harmonicDb(3, 4) < -40.0); // 75 %: same magnitudes, inverted
    CHECK(harmonicDb(0, 4) > -20.0);
    CHECK(harmonicDb(0, 8) < -40.0); // 12.5 %: every 8th missing
}

TEST(noise_lfsr_has_documented_sequence_lengths)
{
    const auto cycleLength = [](bool shortMode)
    {
        // Short mode has separate cycles, so measure from a state already on the cycle.
        std::uint16_t start = 1;
        for (int i = 0; i < 40000; ++i)
            start = nes::stepLfsr(start, shortMode);
        std::uint16_t state = start;
        int length = 0;
        do
        {
            state = nes::stepLfsr(state, shortMode);
            ++length;
        } while (state != start && length < 70000);
        return length;
    };

    CHECK(cycleLength(false) == 32767);
    const int shortLength = cycleLength(true);
    CHECK(shortLength == 93 || shortLength == 31);
}

TEST(noise_note_maps_to_period_index)
{
    CHECK(nes::noiseIndexForNote(60) == 7);
    CHECK(nes::noiseIndexForNote(61) == 6);
    CHECK(nes::noiseIndexForNote(67) == 0);
    CHECK(nes::noiseIndexForNote(100) == 0);
    CHECK(nes::noiseIndexForNote(52) == 15);
    CHECK(nes::noiseIndexForNote(0) == 15);
    CHECK(nes::kNoisePeriods[0] == 4);
    CHECK(nes::kNoisePeriods[15] == 4068);
}

TEST(noise_higher_note_changes_faster)
{
    const auto crossings = [](int note)
    {
        auto engine = makeEngine(NesChannel::Noise);
        engine.noteOn(note, 127);
        const auto out = render(engine, 48000);
        int count = 0;
        for (size_t i = 1; i < out.size(); ++i)
            count += (out[i - 1] < 0.0f) != (out[i] < 0.0f);
        return count;
    };
    CHECK(crossings(60) > 2 * crossings(52));
}

TEST(noise_ignores_pitch_bend)
{
    auto a = makeEngine(NesChannel::Noise);
    auto b = makeEngine(NesChannel::Noise);
    b.setPitchBend(2.0);
    a.noteOn(60, 127);
    b.noteOn(60, 127);
    const auto outA = render(a, 4800);
    const auto outB = render(b, 4800);
    for (size_t i = 0; i < outA.size(); ++i)
        CHECK(outA[i] == outB[i]);
}

TEST(attack_ramps_up)
{
    NesEngine engine;
    NesParams params;
    params.attackMs = 500.0f;
    engine.setParams(params);
    engine.prepare(48000.0);
    engine.noteOn(69, 127);
    const auto early = render(engine, 4800);
    render(engine, 24000);
    const auto late = render(engine, 4800);
    CHECK(rms(early) < 0.5 * rms(late));
}

TEST(decay_settles_on_sustain_level)
{
    const auto level = [](float sustain)
    {
        NesEngine engine;
        NesParams params;
        params.sustain = sustain;
        params.decayMs = 50.0f;
        engine.setParams(params);
        engine.prepare(48000.0);
        engine.noteOn(69, 127);
        render(engine, 12000);
        return rms(render(engine, 4800));
    };
    CHECK(level(0.0f) < 1e-4);
    CHECK(level(4.0f) < 0.6 * level(15.0f));
    CHECK(level(4.0f) > 0.1 * level(15.0f));
}

TEST(release_ends_voice)
{
    NesEngine engine;
    NesParams params;
    params.releaseMs = 100.0f;
    engine.setParams(params);
    engine.prepare(48000.0);
    engine.noteOn(69, 127);
    render(engine, 4800);
    engine.noteOff(69);
    CHECK(engine.activeVoices() == 1);
    render(engine, 9600);
    CHECK(engine.activeVoices() == 0);
}

TEST(velocity_scales_pulse_but_not_triangle)
{
    const auto level = [](NesChannel channel, int velocity)
    {
        auto engine = makeEngine(channel);
        engine.noteOn(69, velocity);
        return rms(render(engine, 9600));
    };
    CHECK(level(NesChannel::Pulse, 40) < 0.6 * level(NesChannel::Pulse, 127));
    CHECK_NEAR(level(NesChannel::Triangle, 40), level(NesChannel::Triangle, 127), 1e-4);
}

TEST(velocity_can_be_disabled)
{
    const auto level = [](int velocity)
    {
        NesEngine engine;
        NesParams params;
        params.velocityOn = false;
        engine.setParams(params);
        engine.prepare(48000.0);
        engine.noteOn(69, velocity);
        return rms(render(engine, 9600));
    };
    CHECK_NEAR(level(40), level(127), 1e-4);
}

TEST(triangle_finishes_at_zero_level_after_note_off)
{
    auto engine = makeEngine(NesChannel::Triangle);
    engine.noteOn(40, 127);
    render(engine, 4800);
    engine.noteOff(40);
    CHECK(engine.activeVoices() == 1);
    render(engine, 4800);
    CHECK(engine.activeVoices() == 0);
}

TEST(ninth_note_steals_oldest_voice)
{
    auto engine = makeEngine(NesChannel::Pulse);
    for (int note = 60; note < 69; ++note)
    {
        engine.noteOn(note, 100);
        render(engine, 100);
    }
    CHECK(engine.activeVoices() == 8);
    CHECK(!engine.isNoteActive(60));
    CHECK(engine.isNoteActive(61));
    CHECK(engine.isNoteActive(68));
}

TEST(held_note_reuses_its_voice)
{
    auto engine = makeEngine(NesChannel::Pulse);
    engine.noteOn(60, 100);
    engine.noteOn(60, 100);
    CHECK(engine.activeVoices() == 1);
}

TEST(sustain_pedal_holds_until_lifted)
{
    auto engine = makeEngine(NesChannel::Pulse);
    engine.setSustain(true);
    engine.noteOn(60, 100);
    engine.noteOff(60);
    render(engine, 4800);
    CHECK(engine.isNoteActive(60));

    engine.noteOn(60, 100); // retrigger during the pedal
    CHECK(engine.activeVoices() == 1);
    render(engine, 4800);
    engine.noteOff(60);
    render(engine, 4800);
    CHECK(engine.isNoteActive(60));

    engine.setSustain(false);
    render(engine, 4800);
    CHECK(!engine.isNoteActive(60));
}

TEST(pedal_lift_keeps_physically_held_notes)
{
    auto engine = makeEngine(NesChannel::Pulse);
    engine.setSustain(true);
    engine.noteOn(60, 100);
    engine.setSustain(false);
    render(engine, 4800);
    CHECK(engine.isNoteActive(60));
}

TEST(pitch_bend_shifts_up_two_semitones)
{
    auto engine = makeEngine(NesChannel::Pulse);
    engine.noteOn(69, 127);
    engine.setPitchBend(2.0);
    const auto out = render(engine, 96000);
    CHECK_NEAR(measureFrequency(out, 48000.0, 4800), pulseFrequency(71), 1.0);
}

TEST(vibrato_moves_pitch_only_when_enabled)
{
    const auto spread = [](float depth)
    {
        NesEngine engine;
        NesParams params;
        params.vibratoDepthCents = depth;
        params.vibratoRateHz = 5.0f;
        engine.setParams(params);
        engine.prepare(48000.0);
        engine.noteOn(69, 127);
        double low = 1e9, high = 0.0;
        for (int i = 0; i < 25; ++i)
        {
            const double f = measureFrequency(render(engine, 2400), 48000.0);
            low = std::min(low, f);
            high = std::max(high, f);
        }
        return high - low;
    };
    CHECK(spread(0.0f) < 8.0);
    CHECK(spread(100.0f) > 35.0); // full swing at A4 is 51 Hz, windows average it
}

TEST(glide_moves_from_previous_note)
{
    NesEngine engine;
    NesParams params;
    params.releaseMs = 0.0f;
    params.glideMs = 1000.0f;
    engine.setParams(params);
    engine.prepare(48000.0);
    engine.noteOn(57, 127);
    engine.noteOff(57);
    render(engine, 4800);
    engine.noteOn(69, 127);
    const double early = measureFrequency(render(engine, 4800), 48000.0);
    render(engine, 48000);
    const double late = measureFrequency(render(engine, 9600), 48000.0);
    CHECK(early < 280.0);
    CHECK_NEAR(late, pulseFrequency(69), 2.0);
}

TEST(prepare_forgets_the_note_a_glide_would_start_from)
{
    NesEngine engine;
    NesParams params;
    params.releaseMs = 0.0f;
    params.glideMs = 1000.0f;
    engine.setParams(params);
    engine.prepare(48000.0);
    engine.noteOn(57, 127);
    render(engine, 4800);

    engine.prepare(48000.0);
    engine.noteOn(69, 127);
    CHECK_NEAR(measureFrequency(render(engine, 4800), 48000.0), pulseFrequency(69), 2.0);
}

TEST(output_has_no_dc)
{
    auto engine = makeEngine(NesChannel::Triangle);
    engine.noteOn(69, 127);
    render(engine, 48000);
    const auto out = render(engine, 48000);
    double mean = 0.0;
    for (auto s : out)
        mean += s;
    mean /= static_cast<double>(out.size());
    CHECK(std::abs(mean) < 0.01 * peak(out));
}

TEST(aliasing_stays_below_minus_50_db)
{
    constexpr double sampleRate = 44100.0;
    auto engine = makeEngine(NesChannel::Pulse, sampleRate);
    engine.noteOn(96, 127);
    render(engine, 4410);
    const auto out = render(engine, 88200);
    const double f = pulseFrequency(96);
    const double reference = magnitudeAt(out, f, sampleRate);

    double worst = -200.0;
    for (int harmonic = 11; harmonic < 60; ++harmonic)
    {
        double folded = std::fmod(harmonic * f, sampleRate);
        if (folded > sampleRate / 2.0)
            folded = sampleRate - folded;
        const double nearest = std::round(folded / f) * f;
        if (folded > 14000.0 || std::abs(folded - nearest) < 40.0)
            continue;
        worst = std::max(worst, decibels(magnitudeAt(out, folded, sampleRate) / reference));
    }
    std::printf("  worst alias: %.1f dB\n", worst);
    CHECK(worst < -50.0);
}

TEST(output_is_finite_at_all_sample_rates)
{
    for (double sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (auto channel : { NesChannel::Pulse, NesChannel::Triangle, NesChannel::Noise })
        {
            auto engine = makeEngine(channel, sampleRate);
            for (int note : { 0, 40, 69, 127 })
                engine.noteOn(note, 127);
            engine.setPitchBend(2.0);
            const auto out = render(engine, 48000);
            CHECK(allFinite(out));
            CHECK(peak(out) > 0.0);
        }
    }
}

TEST(render_before_prepare_is_silent)
{
    NesEngine engine;
    engine.noteOn(69, 127);
    std::vector<float> out(512, 1.0f);
    engine.render(out.data(), 512);
    CHECK(peak(out) == 0.0);
}

TEST(sample_rate_change_stays_in_tune)
{
    auto engine = makeEngine(NesChannel::Pulse, 44100.0);
    engine.noteOn(69, 127);
    render(engine, 1000);
    engine.prepare(96000.0);
    CHECK(engine.activeVoices() == 0);
    engine.noteOn(69, 127);
    const auto out = render(engine, 192000);
    CHECK_NEAR(measureFrequency(out, 96000.0, 9600), pulseFrequency(69), 0.5);
}

TEST(block_size_does_not_change_output)
{
    auto whole = makeEngine(NesChannel::Pulse);
    auto split = makeEngine(NesChannel::Pulse);
    whole.noteOn(69, 127);
    split.noteOn(69, 127);

    const auto wholeOut = render(whole, 20000);
    std::vector<float> splitOut(20000);
    split.render(splitOut.data(), 0);
    for (int i = 0; i < 100; ++i)
        split.render(splitOut.data() + i * 200, 200);

    for (size_t i = 0; i < wholeOut.size(); ++i)
        CHECK_NEAR(wholeOut[i], splitOut[i], 1e-3);
}

TEST(channel_change_with_held_notes_is_safe)
{
    auto engine = makeEngine(NesChannel::Pulse);
    engine.noteOn(60, 127);
    engine.noteOn(64, 127);
    render(engine, 2400);
    NesParams params;
    params.channel = NesChannel::Noise;
    engine.setParams(params);
    CHECK(engine.activeVoices() == 0);
    const auto after = render(engine, 48000);
    CHECK(allFinite(after));
    engine.noteOff(60);
    engine.noteOn(60, 127);
    CHECK(engine.activeVoices() == 1);
    CHECK(rms(render(engine, 2400)) > 0.0);
}

TEST(invalid_events_are_ignored)
{
    auto engine = makeEngine(NesChannel::Pulse);
    engine.noteOn(-1, 100);
    engine.noteOn(128, 100);
    engine.noteOff(60);
    CHECK(engine.activeVoices() == 0);
    engine.noteOn(60, 0); // velocity 0 is a note-off
    CHECK(engine.activeVoices() == 0);
}

TEST(all_notes_off_stops_everything)
{
    auto engine = makeEngine(NesChannel::Pulse);
    engine.setSustain(true);
    engine.noteOn(60, 100);
    engine.noteOn(64, 100);
    engine.allNotesOff();
    CHECK(engine.activeVoices() == 0);
}

TEST(eight_noise_voices_run_well_under_real_time)
{
    constexpr double sampleRate = 44100.0;
    auto engine = makeEngine(NesChannel::Noise, sampleRate);
    for (int note = 67; note < 75; ++note)
        engine.noteOn(note, 127);

    std::vector<float> block(512);
    const auto start = std::chrono::steady_clock::now();
    constexpr int seconds = 10;
    const int blocks = static_cast<int>(seconds * sampleRate / 512);
    for (int i = 0; i < blocks; ++i)
        engine.render(block.data(), 512);
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    std::printf("  CPU: %.1f %% of real time\n", 100.0 * elapsed.count() / seconds);
    CHECK(elapsed.count() < 0.25 * seconds);
}

TEST(resampler_keeps_sine_frequency)
{
    for (double inputRate : { 53267.0, 32000.0 })
    {
        for (double outputRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            Resampler resampler;
            resampler.prepare(inputRate, outputRate);
            const auto out = resample(resampler, sine(1000.0, inputRate, static_cast<int>(inputRate)));
            CHECK(out.size() > static_cast<size_t>(outputRate * 0.9));
            CHECK_NEAR(measureFrequency(out, outputRate, 2000), 1000.0, 1.0);
        }
    }
}

TEST(resampler_alias_below_minus_50_db)
{
    // Tones between the output and input Nyquist really fold into the output band. Each is
    // compared with a passband tone through the same conversion.
    struct Case
    {
        double inputRate, outputRate, tone;
    };
    for (const auto& c : { Case { 53267.0, 44100.0, 23600.0 }, Case { 53267.0, 48000.0, 25300.0 } })
    {
        const double folded = c.outputRate - c.tone;
        const int numIn = static_cast<int>(c.inputRate);

        Resampler passband;
        passband.prepare(c.inputRate, c.outputRate);
        const auto ref = resample(passband, sine(10000.0, c.inputRate, numIn));
        const std::vector<float> refSteady(ref.begin() + 1000, ref.end());
        const double reference = magnitudeAt(refSteady, 10000.0, c.outputRate);

        Resampler resampler;
        resampler.prepare(c.inputRate, c.outputRate);
        const auto out = resample(resampler, sine(c.tone, c.inputRate, numIn));
        const std::vector<float> steady(out.begin() + 1000, out.end());
        const double level = decibels(magnitudeAt(steady, folded, c.outputRate) / reference);
        std::printf("  %.0f Hz from %.0f to %.0f (folds to %.0f Hz): %.1f dB\n", c.tone, c.inputRate, c.outputRate, folded, level);
        CHECK(level < -50.0);
    }
}

TEST(resampler_unity_dc)
{
    for (double inputRate : { 53267.0, 32000.0 })
    {
        for (double outputRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            Resampler resampler;
            resampler.prepare(inputRate, outputRate);
            const auto out = resample(resampler, std::vector<float>(8000, 0.5f));
            CHECK(out.size() > 1000);
            // The filter has settled once its whole window sits on real input.
            const auto settled = static_cast<size_t>(Resampler::kTaps * outputRate / inputRate) + 10;
            double worst = 0.0;
            for (size_t i = settled; i < out.size(); ++i)
                worst = std::max(worst, std::abs(static_cast<double>(out[i]) - 0.5));
            CHECK(worst < 1e-3);
        }
    }
}

TEST(resampler_handles_large_and_empty_requests)
{
    Resampler resampler;
    resampler.prepare(53267.0, 44100.0);
    std::vector<float> out(20000);
    CHECK(resampler.inputNeededFor(0) == 0);
    CHECK(resampler.pull(out.data(), 0) == 0);
    CHECK(resampler.pull(out.data(), 100) == 0); // nothing queued yet
    resampler.push(out.data(), 0);

    // 20000 outputs need more input than one push may carry, so feed it in chunks.
    const std::vector<float> chunk(static_cast<size_t>(Resampler::kMaxChunk), 0.25f);
    int needed = resampler.inputNeededFor(20000);
    CHECK(needed > Resampler::kMaxChunk);
    CHECK(needed < 30000);
    for (int remaining = needed; remaining > 0; remaining -= Resampler::kMaxChunk)
        resampler.push(chunk.data(), std::min(remaining, Resampler::kMaxChunk));
    CHECK(resampler.inputNeededFor(20000) == 0 || resampler.inputNeededFor(20000) <= 1);
    CHECK(resampler.pull(out.data(), 20000) == 20000);

    // Asking for more than what is queued returns only what exists.
    resampler.reset();
    resampler.push(chunk.data(), 500);
    const int available = resampler.pull(out.data(), 20000);
    CHECK(available > 0 && available < 500);
    CHECK(resampler.pull(out.data(), 20000) == 0);

    // Extra input stays queued for the next pull.
    resampler.reset();
    resampler.push(chunk.data(), Resampler::kMaxChunk);
    resampler.push(chunk.data(), Resampler::kMaxChunk);
    const int first = resampler.pull(out.data(), 1000);
    const int second = resampler.pull(out.data(), 20000);
    CHECK(first == 1000);
    CHECK(second > 1000);
}

TEST(resampler_request_beyond_queue_capacity_loses_nothing)
{
    constexpr double inputRate = 53267.0, outputRate = 44100.0, tone = 1000.0;
    constexpr int total = 100000; // needs far more input than the queue holds
    Resampler resampler;
    resampler.prepare(inputRate, outputRate);
    CHECK(resampler.inputNeededFor(total) < 6 * Resampler::kMaxChunk);

    std::vector<float> out;
    std::vector<float> block(static_cast<size_t>(Resampler::kMaxChunk));
    std::vector<float> chunk;
    long long inputIndex = 0;
    while (static_cast<int>(out.size()) < total)
    {
        for (int needed = resampler.inputNeededFor(total - static_cast<int>(out.size())); needed > 0;)
        {
            const int n = std::min(needed, Resampler::kMaxChunk);
            chunk.resize(static_cast<size_t>(n));
            for (auto& v : chunk)
                v = static_cast<float>(std::sin(2.0 * kPi * tone * static_cast<double>(inputIndex++) / inputRate));
            resampler.push(chunk.data(), n);
            needed -= n;
        }
        const int want = std::min(static_cast<int>(block.size()), total - static_cast<int>(out.size()));
        const int got = resampler.pull(block.data(), want);
        CHECK(got > 0);
        if (got == 0)
            break;
        out.insert(out.end(), block.begin(), block.begin() + got);
    }
    CHECK(static_cast<int>(out.size()) == total);
    // A dropped stretch of input would shift the phase and change the measured frequency.
    CHECK_NEAR(measureFrequency(out, outputRate, 2000), tone, 0.05);
}

TEST(resampler_latency_matches_impulse)
{
    for (double inputRate : { 53267.0, 32000.0 })
    {
        for (double outputRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            Resampler resampler;
            resampler.prepare(inputRate, outputRate);
            constexpr int impulseAt = 100;
            std::vector<float> in(400, 0.0f);
            in[impulseAt] = 1.0f;
            const auto out = resample(resampler, in);
            size_t best = 0;
            for (size_t i = 0; i < out.size(); ++i)
                if (std::abs(out[i]) > std::abs(out[best]))
                    best = i;
            const double expected = impulseAt * outputRate / inputRate + resampler.latencyOutputSamples();
            CHECK(std::abs(static_cast<double>(best) - expected) <= 2.0);
            CHECK(resampler.latencyOutputSamples() > 0);
        }
    }
}

TEST(genesis_pitch_across_keyboard)
{
    for (int note : { 36, 60, 69, 84, 96 })
    {
        auto engine = makeGenesis(paramsForPatch(kPatchPureTone));
        engine->noteOn(note, 127);
        const auto out = render(*engine, 96000);
        const double cents = centsBetween(measureFrequency(out, 48000.0, 4800), noteFrequency(note));
        std::printf("  note %d: %+.2f cents\n", note, cents);
        CHECK(std::abs(cents) < 10.0);
    }
}

TEST(genesis_clock_gives_expected_rate)
{
    // A wrong divisor moves every note by the same ratio, whatever the host's rate.
    CHECK(GenesisEngine::kClock == 7670453);
    CHECK_NEAR(GenesisEngine::kChipRate, 7670453.0 / 144.0, 1e-9);
    for (double sampleRate : { 44100.0, 96000.0 })
    {
        auto engine = makeGenesis(paramsForPatch(kPatchPureTone), sampleRate);
        engine->noteOn(69, 127);
        const auto out = render(*engine, static_cast<int>(2.0 * sampleRate));
        const double cents = centsBetween(measureFrequency(out, sampleRate, static_cast<size_t>(sampleRate / 10.0)), 440.0);
        std::printf("  %.0f Hz: %+.2f cents\n", sampleRate, cents);
        CHECK(std::abs(cents) < 10.0);
    }
}

TEST(genesis_algorithm_changes_spectrum)
{
    auto serial = paramsForPatch(kPatchLead);
    auto parallel = serial;
    serial.algorithm = 0;
    parallel.algorithm = 7;
    const double a = heldHarmonicDb(serial), b = heldHarmonicDb(parallel);
    std::printf("  algorithm 0: %.1f dB, algorithm 7: %.1f dB\n", a, b);
    CHECK(std::abs(a - b) >= 3.0);
}

TEST(genesis_feedback_adds_harmonics)
{
    // Operator 1 reaches the output directly in Organ, so feedback can only add to its sine.
    auto none = paramsForPatch(kPatchOrgan);
    auto full = none;
    none.feedback = 0;
    full.feedback = 7;
    const double a = heldHarmonicDb(none), b = heldHarmonicDb(full);
    std::printf("  feedback 0: %.1f dB, feedback 7: %.1f dB\n", a, b);
    CHECK(b - a >= 3.0);
}

TEST(genesis_brightness_adds_harmonics)
{
    auto dark = paramsForPatch(kPatchLead);
    auto bright = dark;
    dark.brightness = -1.0f;
    bright.brightness = 1.0f;
    const double a = heldHarmonicDb(dark), b = heldHarmonicDb(bright);
    std::printf("  brightness -1: %.1f dB, +1: %.1f dB\n", a, b);
    CHECK(b - a >= 3.0);

    // Macros reach a note that is already sounding, at the next 60 Hz step.
    auto engine = makeGenesis(dark);
    engine->noteOn(57, 127);
    render(*engine, 24000);
    const double before = harmonicDb(render(*engine, 24000), noteFrequency(57), 48000.0);
    engine->setParams(bright);
    render(*engine, 2400);
    const double after = harmonicDb(render(*engine, 24000), noteFrequency(57), 48000.0);
    std::printf("  same note, before: %.1f dB, after: %.1f dB\n", before, after);
    CHECK(after - before >= 3.0);
}

TEST(genesis_note_off_ends)
{
    for (int patch = 0; patch < 12; ++patch)
    {
        auto engine = makeGenesis(paramsForPatch(patch));
        engine->noteOn(60, 127);
        const double reference = loudestWindowRms(render(*engine, 48000));
        engine->noteOff(60);
        CHECK(engine->activeVoices() == 1);
        render(*engine, 48000 * 19 / 10);
        CHECK(engine->activeVoices() == 1); // a key-off voice counts as active for 2 s
        render(*engine, 48000 * 3 / 5);     // now 2.5 s after the key went up
        CHECK(engine->activeVoices() == 0);
        CHECK(!engine->isNoteActive(60));
        const double tail = decibels(rms(render(*engine, 4800)) / reference);
        std::printf("  %s: tail %.1f dB\n", kFmPatches[static_cast<size_t>(patch)].name, tail);
        CHECK(tail < -60.0);
    }

    // Even the slowest release can't outlive the 2 s a voice counts as active.
    auto slowest = paramsForPatch(kPatchPad);
    slowest.release = 1.0f;
    auto engine = makeGenesis(slowest);
    engine->noteOn(60, 127);
    const double reference = loudestWindowRms(render(*engine, 96000));
    engine->noteOff(60);
    render(*engine, 120000);
    CHECK(engine->activeVoices() == 0);
    CHECK(decibels(rms(render(*engine, 4800)) / reference) < -60.0);
}

TEST(genesis_release_cap_finishes_without_a_click)
{
    const auto largestStep = [](const std::vector<float>& x)
    {
        double largest = 0.0;
        for (size_t i = 1; i < x.size(); ++i)
            largest = std::max(largest, std::abs(static_cast<double>(x[i]) - x[i - 1]));
        return largest;
    };

    struct Case
    {
        int patch;
        float release;
        bool macroChangeNearTheEnd; // must not bring the slow release back
    };
    for (const auto& c : { Case { kPatchPureTone, 1.0f, false }, Case { kPatchPureTone, 0.5f, false }, Case { kPatchLead, 1.0f, false },
                           Case { kPatchPad, 0.5f, false }, Case { kPatchPureTone, 1.0f, true } })
    {
        auto params = paramsForPatch(c.patch);
        params.release = c.release;
        auto engine = makeGenesis(params);
        engine->noteOn(60, 127);
        const double reference = loudestWindowRms(render(*engine, 48000));
        engine->noteOff(60);

        render(*engine, 48000);
        const double normal = largestStep(render(*engine, 40800)); // 1 to 1.85 s after key-off

        std::vector<float> nearEnd(static_cast<size_t>(16800)); // 1.85 to 2.2 s
        if (c.macroChangeNearTheEnd)
        {
            // A change before every 60 Hz step, so one lands on the step that starts the finish.
            auto changed = params;
            for (int done = 0; done < 16800; done += 100)
            {
                changed.brightness = changed.brightness > 0.0f ? 0.0f : 0.5f;
                engine->setParams(changed);
                engine->render(nearEnd.data() + done, 100);
            }
        }
        else
        {
            engine->render(nearEnd.data(), 16800);
        }
        const double atEnd = largestStep(nearEnd);

        render(*engine, 10560); // 2.42 s
        CHECK(engine->activeVoices() == 0);
        const double tail = decibels(rms(render(*engine, 3840)) / reference); // up to 2.5 s
        std::printf("  %s, release %+.1f%s: largest step %.4f, then %.4f near 2 s; tail %.1f dB\n", kFmPatches[static_cast<size_t>(c.patch)].name,
                    c.release, c.macroChangeNearTheEnd ? ", macro change" : "", normal, atEnd, tail);
        CHECK(atEnd < 1.5 * normal);
        CHECK(tail < -60.0);
    }

    // The next note on that voice gets its full release back.
    auto params = paramsForPatch(kPatchPureTone);
    params.release = 1.0f;
    auto engine = makeGenesis(params);
    engine->noteOn(60, 127);
    render(*engine, 4800);
    engine->noteOff(60);
    render(*engine, 48000 * 195 / 100); // inside the last 100 ms
    CHECK(engine->isNoteActive(60));
    engine->noteOn(60, 127);
    const double held = rms(render(*engine, 24000));
    engine->noteOff(60);
    render(*engine, 2400);
    const double released = rms(render(*engine, 2400));
    std::printf("  next note: held %.4f, 50 ms after key-off %.4f\n", held, released);
    CHECK(released > 0.8 * held);
}

TEST(genesis_ninth_note_steals_oldest)
{
    auto engine = makeGenesis(paramsForPatch(kPatchPureTone));
    for (int note = 60; note < 69; ++note)
    {
        engine->noteOn(note, 100);
        render(*engine, 100);
    }
    CHECK(engine->activeVoices() == 8);
    CHECK(!engine->isNoteActive(60));
    CHECK(engine->isNoteActive(61));
    CHECK(engine->isNoteActive(68));

    // A voice that has finished is taken before the oldest held one.
    auto free = makeGenesis(paramsForPatch(kPatchPureTone));
    for (int note = 60; note < 68; ++note)
        free->noteOn(note, 100);
    free->noteOff(63);
    render(*free, 48000 * 21 / 10);
    CHECK(free->activeVoices() == 7);
    free->noteOn(70, 100);
    CHECK(free->activeVoices() == 8);
    CHECK(free->isNoteActive(60));
    CHECK(free->isNoteActive(70));
    CHECK(!free->isNoteActive(63));

    // A held note reuses its voice.
    auto held = makeGenesis(paramsForPatch(kPatchPureTone));
    held->noteOn(60, 100);
    held->noteOn(60, 100);
    CHECK(held->activeVoices() == 1);
}

TEST(genesis_stealing_works_on_both_chips)
{
    const int notes[] = { 48, 52, 55, 60, 64, 67, 72, 76 };
    auto engine = makeGenesis(paramsForPatch(kPatchPureTone));
    for (int note : notes)
    {
        engine->noteOn(note, 127);
        render(*engine, 480);
    }

    // Replaying the first chip's six notes makes the second chip's two voices the oldest.
    for (int i = 0; i < 6; ++i)
    {
        engine->noteOn(notes[i], 127);
        render(*engine, 480);
    }

    auto single = makeGenesis(paramsForPatch(kPatchPureTone));
    single->noteOn(84, 127);
    render(*single, 4800);
    const double reference = magnitudeAt(render(*single, 48000), noteFrequency(84), 48000.0);

    const auto steal = [&](int stolen, int played)
    {
        CHECK(engine->isNoteActive(stolen));
        engine->noteOn(played, 127);
        CHECK(engine->activeVoices() == 8);
        CHECK(!engine->isNoteActive(stolen));
        CHECK(engine->isNoteActive(played));

        render(*engine, 4800);
        const auto out = render(*engine, 48000);
        const double gone = decibels(magnitudeAt(out, noteFrequency(stolen), 48000.0) / reference);
        const double heard = decibels(magnitudeAt(out, noteFrequency(played), 48000.0) / reference);
        std::printf("  note %d stolen by %d: stolen %.1f dB, new %+.2f dB\n", stolen, played, gone, heard);
        CHECK(gone < -40.0);
        CHECK_NEAR(heard, 0.0, 1.0);
    };

    steal(72, 84); // channel 0 of the second chip
    steal(76, 87); // channel 1 of the second chip
    steal(48, 90); // then back to the first chip, its oldest being the first note replayed
}

TEST(genesis_all_8_voices_sound_on_both_chips)
{
    // Voices 4 to 6 use the first chip's upper port and voices 7 and 8 the second chip. Every
    // note must still be there at the end, so no voice can be writing over another's registers.
    const int notes[] = { 48, 52, 55, 60, 64, 67, 72, 76 };
    auto engine = makeGenesis(paramsForPatch(kPatchPureTone));
    for (int note : notes)
    {
        engine->noteOn(note, 127);
        render(*engine, 480);
    }
    CHECK(engine->activeVoices() == 8);
    render(*engine, 4800);
    const auto chord = render(*engine, 48000);

    auto single = makeGenesis(paramsForPatch(kPatchPureTone));
    single->noteOn(notes[0], 127);
    render(*single, 4800);
    const double reference = magnitudeAt(render(*single, 48000), noteFrequency(notes[0]), 48000.0);

    for (int note : notes)
    {
        const double level = decibels(magnitudeAt(chord, noteFrequency(note), 48000.0) / reference);
        std::printf("  note %d: %+.2f dB relative to one voice\n", note, level);
        CHECK_NEAR(level, 0.0, 1.0);
    }
}

TEST(genesis_retrigger_restarts_attack)
{
    auto engine = makeGenesis(paramsForPatch(kPatchBell));
    engine->noteOn(60, 127);
    const double start = rms(render(*engine, 2400));
    render(*engine, 96000);
    const double decayed = rms(render(*engine, 2400));
    CHECK(decayed < 0.5 * start);

    // Key off and key on arrive together: the chip must still see the edge.
    engine->noteOn(60, 127);
    const double again = rms(render(*engine, 2400));
    std::printf("  start %.4f, decayed %.4f, retriggered %.4f\n", start, decayed, again);
    CHECK(again > 0.8 * start);
    CHECK(engine->activeVoices() == 1);
}

TEST(genesis_note_off_right_after_note_on_still_sounds)
{
    auto engine = makeGenesis(paramsForPatch(kPatchPureTone));
    engine->noteOn(60, 127);
    engine->noteOff(60);
    const double level = rms(render(*engine, 2400));
    std::printf("  level %.4f\n", level);
    CHECK(level > 0.01);
    render(*engine, 48000 * 21 / 10);
    CHECK(engine->activeVoices() == 0);
}

TEST(genesis_sustain_and_bend)
{
    auto engine = makeGenesis(paramsForPatch(kPatchPureTone));
    engine->setSustain(true);
    engine->noteOn(60, 100);
    const double held = rms(render(*engine, 4800));
    engine->noteOff(60);
    render(*engine, 48000 * 3);
    CHECK(engine->isNoteActive(60)); // well past the 2 s of a released voice
    CHECK_NEAR(rms(render(*engine, 4800)), held, 0.1 * held);
    engine->setSustain(false);
    render(*engine, 48000 * 5 / 2);
    CHECK(!engine->isNoteActive(60));
    CHECK(rms(render(*engine, 4800)) < 0.001 * held);

    // Lifting the pedal leaves keys that are physically down.
    engine->setSustain(true);
    engine->noteOn(62, 100);
    engine->setSustain(false);
    render(*engine, 48000 * 5 / 2);
    CHECK(engine->isNoteActive(62));
    engine->allNotesOff();

    auto bent = makeGenesis(paramsForPatch(kPatchPureTone));
    bent->noteOn(69, 127);
    render(*bent, 4800);
    bent->setPitchBend(2.0);
    render(*bent, 4800);
    const double up = centsBetween(measureFrequency(render(*bent, 48000), 48000.0), noteFrequency(71));
    bent->setPitchBend(-2.0);
    render(*bent, 4800);
    const double down = centsBetween(measureFrequency(render(*bent, 48000), 48000.0), noteFrequency(67));
    std::printf("  bend up %+.2f cents, down %+.2f cents\n", up, down);
    CHECK(std::abs(up) < 10.0);
    CHECK(std::abs(down) < 10.0);
}

TEST(genesis_output_level_comparable)
{
    auto engine = makeGenesis(paramsForPatch(kPatchPureTone));
    engine->noteOn(69, 127);
    render(*engine, 4800);
    const double genesisLevel = rms(render(*engine, 48000));

    NesEngine pulse;
    pulse.prepare(48000.0);
    pulse.noteOn(69, 127);
    render(pulse, 4800);
    const double pulseLevel = rms(render(pulse, 48000));

    std::printf("  Pure Tone %.4f, 2A03 pulse %.4f, %+.2f dB\n", genesisLevel, pulseLevel, decibels(genesisLevel / pulseLevel));
    CHECK(std::abs(decibels(genesisLevel / pulseLevel)) < 6.0);
}

TEST(genesis_patches_level_and_decay)
{
    // Measured, since nobody can listen here: every patch sounds, near the level of Pure Tone.
    const auto measure = [](int patch, double& decaySeconds, double& releaseSeconds, double& sustainDb)
    {
        auto held = makeGenesis(paramsForPatch(patch));
        held->noteOn(60, 127);
        const auto note = render(*held, 48000 * 4);
        const double level = loudestWindowRms(note);
        decaySeconds = secondsUntilBelow(note, level * 0.001, 48000.0);
        sustainDb = decibels(rms(std::vector<float>(note.end() - 4800, note.end())) / level);

        auto released = makeGenesis(paramsForPatch(patch));
        released->noteOn(60, 127);
        render(*released, 48000);
        released->noteOff(60);
        releaseSeconds = secondsUntilBelow(render(*released, 48000 * 3), level * 0.001, 48000.0);
        return level;
    };

    double unused = 0.0, sustain = 0.0;
    const double reference = measure(kPatchPureTone, unused, unused, sustain);
    // -60 dB is relative to the loudest moment. Held: -1 means not within 4 s. Off: from the key
    // going up 1 s after the note began, and 0 means the note was already below it by then.
    std::printf("  %-10s %9s %10s %12s %12s\n", "patch", "rms", "re Pure", "-60 dB held", "-60 dB off");
    for (int patch = 0; patch < 12; ++patch)
    {
        double decaySeconds = 0.0, releaseSeconds = 0.0;
        const double level = measure(patch, decaySeconds, releaseSeconds, sustain);
        std::printf("  %-10s %9.4f %+8.1f dB %10.2f s %10.2f s  (level at 4 s: %.1f dB)\n", kFmPatches[static_cast<size_t>(patch)].name, level,
                    decibels(level / reference), decaySeconds, releaseSeconds, sustain);
        CHECK(std::abs(decibels(level / reference)) < 6.0);
        CHECK(releaseSeconds >= 0.0 && releaseSeconds < 2.0);
    }
}

TEST(genesis_patch_names_and_ranges)
{
    const char* names[] = { "Pure Tone", "FM Bass", "Slap Bass", "E-Piano", "Bell", "Brass", "Lead", "Organ", "Pad", "Guitar", "Pluck", "Perc" };
    for (size_t i = 0; i < 12; ++i)
    {
        const auto& patch = kFmPatches[i];
        CHECK(std::string_view(patch.name) == names[i]);
        CHECK(patch.algorithm >= 0 && patch.algorithm <= 7);
        CHECK(patch.feedback >= 0 && patch.feedback <= 7);
        for (const auto& op : patch.ops)
        {
            CHECK(op.mul >= 0 && op.mul <= 15);
            CHECK(op.dt >= 0 && op.dt <= 7);
            CHECK(op.tl >= 0 && op.tl <= 127);
            CHECK(op.rs >= 0 && op.rs <= 3);
            CHECK(op.ar >= 0 && op.ar <= 31);
            CHECK(op.dr >= 0 && op.dr <= 31);
            CHECK(op.sr >= 0 && op.sr <= 31);
            CHECK(op.sl >= 0 && op.sl <= 15);
            CHECK(op.rr >= 0 && op.rr <= 15);
        }
    }

    const auto& pure = kFmPatches[kPatchPureTone];
    CHECK(pure.algorithm == 7);
    CHECK(pure.ops[3].mul == 1);
    CHECK(pure.ops[3].tl < 127);
    for (int op = 0; op < 3; ++op)
        CHECK(pure.ops[static_cast<size_t>(op)].tl == 127);
}

TEST(genesis_velocity_offsets_carriers)
{
    const auto level = [](int velocity, bool enabled)
    {
        auto params = paramsForPatch(kPatchPureTone);
        params.velocityOn = enabled;
        auto engine = makeGenesis(params);
        engine->noteOn(69, velocity);
        render(*engine, 4800);
        return rms(render(*engine, 9600));
    };
    // The lowest velocity adds 24 TL steps of 0.75 dB, so 18 dB.
    CHECK_NEAR(decibels(level(1, true) / level(127, true)), -18.0, 1.0);
    CHECK_NEAR(decibels(level(1, false) / level(127, false)), 0.0, 0.1);
}

TEST(genesis_envelope_macros_shift_rates)
{
    // Positive macros mean longer, like the 2A03's time knobs: they lower the chip's rates.
    const auto risingLevel = [](float attack)
    {
        auto params = paramsForPatch(kPatchPad);
        params.attack = attack;
        auto engine = makeGenesis(params);
        engine->noteOn(60, 127);
        return rms(render(*engine, 2400));
    };
    CHECK(risingLevel(-1.0f) > 2.0 * risingLevel(0.0f));
    CHECK(risingLevel(1.0f) < 0.5 * risingLevel(0.0f));

    const auto decayedLevel = [](float decay)
    {
        auto params = paramsForPatch(kPatchBell);
        params.decay = decay;
        auto engine = makeGenesis(params);
        engine->noteOn(60, 127);
        render(*engine, 24000);
        return rms(render(*engine, 2400));
    };
    CHECK(decayedLevel(-0.5f) < 0.5 * decayedLevel(0.0f));
    CHECK(decayedLevel(0.5f) > 1.2 * decayedLevel(0.0f));

    const auto releasedLevel = [](float release)
    {
        auto params = paramsForPatch(kPatchPad);
        params.release = release;
        auto engine = makeGenesis(params);
        engine->noteOn(60, 127);
        render(*engine, 48000);
        engine->noteOff(60);
        render(*engine, 4800);
        return rms(render(*engine, 2400));
    };
    CHECK(releasedLevel(-0.5f) < 0.5 * releasedLevel(0.0f));
    CHECK(releasedLevel(0.5f) > 1.2 * releasedLevel(0.0f));

    // Macros outside -1 to +1, or at the ends, stay inside the chip's ranges and keep sounding.
    auto extreme = paramsForPatch(kPatchLead);
    extreme.attack = extreme.decay = extreme.release = extreme.brightness = 5.0f;
    auto engine = makeGenesis(extreme);
    engine->noteOn(60, 127);
    CHECK(rms(render(*engine, 2400)) > 0.0);
}

TEST(genesis_every_patch_sounds_at_both_ends_of_attack)
{
    // No knob position may silence a note, so both ends must reach the patch's normal level.
    const auto loudest = [](int patch, float attack)
    {
        auto params = paramsForPatch(patch);
        params.attack = attack;
        auto engine = makeGenesis(params);
        engine->noteOn(60, 127);
        return loudestWindowRms(render(*engine, 48000 * 6));
    };
    for (int patch = 0; patch < 12; ++patch)
    {
        const double normal = loudest(patch, 0.0f);
        const double fastest = decibels(loudest(patch, -1.0f) / normal);
        const double slowest = decibels(loudest(patch, 1.0f) / normal);
        std::printf("  %s: attack -100 %% %+.1f dB, +100 %% %+.1f dB\n", kFmPatches[static_cast<size_t>(patch)].name, fastest, slowest);
        CHECK(std::abs(fastest) < 6.0);
        CHECK(std::abs(slowest) < 6.0);
    }
}

TEST(genesis_attack_floor_leaves_long_swells)
{
    // Pad at Attack +100 % sits on the floor, so this measures the longest swell it allows.
    auto params = paramsForPatch(kPatchPad);
    params.attack = 1.0f;
    auto engine = makeGenesis(params);
    engine->noteOn(60, 127);
    const auto out = render(*engine, 48000 * 6);
    const double target = 0.7 * peak(out);
    size_t reached = 0;
    while (reached < out.size() && std::abs(static_cast<double>(out[reached])) < target)
        ++reached;
    const double seconds = static_cast<double>(reached) / 48000.0;
    std::printf("  70 %% of peak at %.3f s\n", seconds);
    CHECK(seconds > 1.0);
    CHECK(seconds < 3.0);
}

TEST(genesis_vibrato_and_glide_move_pitch)
{
    const auto spread = [](float depth)
    {
        auto params = paramsForPatch(kPatchPureTone);
        params.vibratoDepthCents = depth;
        params.vibratoRateHz = 5.0f;
        auto engine = makeGenesis(params);
        engine->noteOn(69, 127);
        render(*engine, 2400);
        double low = 1e9, high = 0.0;
        for (int i = 0; i < 25; ++i)
        {
            const double f = measureFrequency(render(*engine, 2400), 48000.0);
            low = std::min(low, f);
            high = std::max(high, f);
        }
        return high - low;
    };
    CHECK(spread(0.0f) < 2.0);
    CHECK(spread(100.0f) > 35.0); // full swing at A4 is 51 Hz, windows average it

    auto params = paramsForPatch(kPatchPureTone);
    params.glideMs = 1000.0f;
    auto engine = makeGenesis(params);
    engine->noteOn(57, 127);
    engine->noteOff(57);
    render(*engine, 4800);
    engine->noteOn(69, 127);
    const double early = measureFrequency(render(*engine, 4800), 48000.0);
    render(*engine, 48000);
    const double late = measureFrequency(render(*engine, 9600), 48000.0);
    CHECK(early < 280.0);
    CHECK(std::abs(centsBetween(late, 440.0)) < 10.0);
}

TEST(genesis_stops_notes_on_patch_change_and_all_notes_off)
{
    auto engine = makeGenesis(paramsForPatch(kPatchPureTone));
    engine->setSustain(true);
    engine->noteOn(60, 127);
    engine->noteOn(64, 127);
    render(*engine, 2400);
    engine->allNotesOff();
    CHECK(engine->activeVoices() == 0);
    render(*engine, 4800); // the DC blocker settles
    CHECK(rms(render(*engine, 2400)) < 1e-3);
    engine->setSustain(false);

    engine->noteOn(60, 127);
    render(*engine, 2400);
    engine->setParams(paramsForPatch(kPatchBell));
    CHECK(engine->activeVoices() == 0);
    render(*engine, 4800);
    CHECK(rms(render(*engine, 2400)) < 1e-3);

    // A note after the stop plays normally, and other parameters leave notes alone.
    engine->noteOn(60, 127);
    CHECK(rms(render(*engine, 2400)) > 0.01);
    auto sameBell = paramsForPatch(kPatchBell);
    sameBell.brightness = 0.5f;
    engine->setParams(sameBell);
    CHECK(engine->activeVoices() == 1);
}

TEST(genesis_invalid_events_are_ignored)
{
    auto engine = makeGenesis(paramsForPatch(kPatchPureTone));
    engine->noteOn(-1, 100);
    engine->noteOn(128, 100);
    engine->noteOff(60);
    CHECK(engine->activeVoices() == 0);
    engine->noteOn(60, 100);
    engine->noteOn(60, 0); // velocity 0 is a note-off
    CHECK(engine->isNoteActive(60));
    engine->allNotesOff();

    GenesisParams wild;
    wild.patch = 99;
    wild.algorithm = 99;
    wild.feedback = -3;
    wild.brightness = -7.0f;
    engine->setParams(wild);
    engine->noteOn(60, 100);
    CHECK(allFinite(render(*engine, 2400)));
}

TEST(genesis_finite_at_all_rates)
{
    for (double sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (int patch = 0; patch < 12; ++patch)
        {
            auto engine = makeGenesis(paramsForPatch(patch), sampleRate);
            for (int note : { 0, 40, 69, 127 })
                engine->noteOn(note, 127);
            engine->setPitchBend(2.0);
            const auto out = render(*engine, 24000);
            CHECK(allFinite(out));
            CHECK(peak(out) > 0.0);
        }
        CHECK(makeGenesis(paramsForPatch(kPatchPureTone), sampleRate)->latencySamples() > 0);
    }
}

TEST(genesis_large_and_empty_blocks)
{
    for (double sampleRate : { 44100.0, 192000.0 })
    {
        auto whole = makeGenesis(paramsForPatch(kPatchPureTone), sampleRate);
        auto split = makeGenesis(paramsForPatch(kPatchPureTone), sampleRate);
        whole->noteOn(69, 127);
        split->noteOn(69, 127);

        // Far more than one resampler chunk, and more than its queue holds.
        constexpr int total = 200000;
        const auto wholeOut = render(*whole, total);
        std::vector<float> splitOut(total, 0.0f);
        split->render(splitOut.data(), 0);
        for (int done = 0, block = 1; done < total; block = block % 1000 + 37)
        {
            const int n = std::min(block, total - done);
            split->render(splitOut.data() + done, n);
            done += n;
        }

        double worst = 0.0;
        for (size_t i = 0; i < wholeOut.size(); ++i)
            worst = std::max(worst, std::abs(static_cast<double>(wholeOut[i]) - splitOut[i]));
        CHECK(worst < 1e-6);
        CHECK(std::abs(centsBetween(measureFrequency(wholeOut, sampleRate, 2000), 440.0)) < 10.0);
    }
}

TEST(genesis_8_voices_under_25_percent_cpu)
{
    for (double sampleRate : { 44100.0, 192000.0 })
    {
        auto params = paramsForPatch(kPatchOrgan); // all 4 operators of all 8 voices keep running
        params.vibratoDepthCents = 20.0f;          // keeps the 60 Hz register writes busy
        auto engine = makeGenesis(params, sampleRate);
        for (int note = 60; note < 68; ++note)
            engine->noteOn(note, 127);

        std::vector<float> block(512);
        const auto start = std::chrono::steady_clock::now();
        constexpr int seconds = 10;
        const int blocks = static_cast<int>(seconds * sampleRate / 512);
        for (int i = 0; i < blocks; ++i)
            engine->render(block.data(), 512);
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        std::printf("  %.0f Hz: CPU %.1f %% of one core\n", sampleRate, 100.0 * elapsed.count() / seconds);
        CHECK(peak(block) > 0.01); // the 8 voices are still sounding
        CHECK(engine->activeVoices() == 8);
        CHECK(elapsed.count() < 0.25 * seconds);
    }
}

TEST(genesis_silent_before_prepare)
{
    GenesisEngine engine;
    engine.noteOn(69, 127);
    CHECK(engine.activeVoices() == 0);
    std::vector<float> out(512, 1.0f);
    engine.render(out.data(), 512);
    CHECK(peak(out) == 0.0);
}

TEST(genesis_silent_after_prepare_has_no_dc)
{
    auto engine = makeGenesis(paramsForPatch(kPatchPureTone));
    CHECK(peak(render(*engine, 9600)) == 0.0); // the chips' idle offset must not reach the output

    engine->noteOn(69, 127);
    render(*engine, 48000);
    const auto out = render(*engine, 48000);
    double mean = 0.0;
    for (auto s : out)
        mean += s;
    mean /= static_cast<double>(out.size());
    CHECK(std::abs(mean) < 0.01 * peak(out));
}

TEST(genesis_prepare_resets_voices)
{
    auto engine = makeGenesis(paramsForPatch(kPatchPureTone), 44100.0);
    engine->noteOn(69, 127);
    render(*engine, 1000);
    engine->prepare(96000.0);
    CHECK(engine->activeVoices() == 0);
    CHECK(peak(render(*engine, 960)) == 0.0);
    engine->noteOn(69, 127);
    const auto out = render(*engine, 192000);
    CHECK(std::abs(centsBetween(measureFrequency(out, 96000.0, 9600), 440.0)) < 10.0);
}

TEST(genesis_audio_thread_calls_do_not_allocate)
{
    auto params = paramsForPatch(kPatchEPiano);
    params.vibratoDepthCents = 30.0f;
    params.glideMs = 100.0f;
    auto engine = makeGenesis(params);
    auto changed = params;
    changed.brightness = 0.7f;
    changed.algorithm = 2;
    changed.feedback = 5;
    changed.attack = -0.5f;
    const auto otherPatch = paramsForPatch(kPatchBell);
    std::vector<float> block(30000);

    const auto before = allocationCount.load();
    for (int note = 50; note < 62; ++note)
    {
        engine->noteOn(note, 100);
        engine->render(block.data(), 64);
    }
    engine->setParams(changed);
    engine->setPitchBend(1.5);
    engine->render(block.data(), 30000); // beyond one resampler chunk
    engine->setSustain(true);
    engine->noteOff(61);
    engine->setSustain(false);
    engine->noteOff(60);
    engine->render(block.data(), 512);
    engine->setParams(otherPatch);
    engine->noteOn(70, 90);
    engine->render(block.data(), 512);
    engine->allNotesOff();
    engine->render(block.data(), 512);
    const auto after = allocationCount.load();

    CHECK(after == before);
}

TEST(gauss_table_is_complete)
{
    const auto& g = snes::kGaussTable;
    CHECK(g.size() == 512);
    for (size_t i = 0; i < 16; ++i)
        CHECK(g[i] == 0);
    constexpr std::array<int, 16> last { 0x513, 0x514, 0x514, 0x515, 0x516, 0x516, 0x517, 0x517,
                                         0x517, 0x518, 0x518, 0x518, 0x518, 0x518, 0x519, 0x519 };
    for (size_t i = 0; i < 16; ++i)
        CHECK(g[496 + i] == last[i]);

    int lowest = 1 << 30, highest = 0;
    for (size_t i = 0; i < 256; ++i)
    {
        const int sum = g[0xFF - i] + g[0x1FF - i] + g[0x100 + i] + g[i];
        lowest = std::min(lowest, sum);
        highest = std::max(highest, sum);
    }
    std::printf("  four taps sum to 0x%X .. 0x%X\n", static_cast<unsigned>(lowest), static_cast<unsigned>(highest));
    CHECK(lowest >= 0x7FF);
    CHECK(highest <= 0x801);
}

TEST(brr_decodes_each_filter)
{
    // Filter 0, nibbles high first. (nibble << shift) >> 1 keeps the sign.
    {
        const auto d = decodeBrr(4, 0, { 1, -1, 7, -8 });
        CHECK(d.out[0] == 8);
        CHECK(d.out[1] == -8);
        CHECK(d.out[2] == 56);
        CHECK(d.out[3] == -64);
        CHECK(d.out[4] == 0);
    }
    {
        const auto d = decodeBrr(0, 0, { 1, -1, 7, -8 }); // shift 0 loses the low bit, rounding down
        CHECK(d.out[0] == 0);
        CHECK(d.out[1] == -1);
        CHECK(d.out[2] == 3);
        CHECK(d.out[3] == -4);
    }
    {
        const auto d = decodeBrr(12, 0, { 7, -8 });
        CHECK(d.out[0] == 14336);
        CHECK(d.out[1] == -16384);
    }

    // Filter 1: 8 + 1000 + (-1000 >> 4 = -63), then 945 + (-945 >> 4 = -60).
    {
        const auto d = decodeBrr(4, 1, { 1, 0 }, 1000, 0);
        CHECK(d.out[0] == 945);
        CHECK(d.out[1] == 885);
    }
    {
        const auto d = decodeBrr(4, 1, { 0 }, -1000, 0); // -1000 + (1000 >> 4 = 62)
        CHECK(d.out[0] == -938);
    }

    // Filter 2: 8 + 2000 + (-3000 >> 5 = -94) - 500 + (500 >> 4 = 31),
    // then 2890 + (-4335 >> 5 = -136) - 1000 + (1000 >> 4 = 62).
    {
        const auto d = decodeBrr(4, 2, { 1, 0 }, 1000, 500);
        CHECK(d.out[0] == 1445);
        CHECK(d.out[1] == 1816);
    }

    // Filter 3: 8 + 2000 + (-13000 >> 6 = -204) - 500 + (1500 >> 4 = 93),
    // then 2794 + (-18161 >> 6 = -284) - 1000 + (3000 >> 4 = 187).
    {
        const auto d = decodeBrr(4, 3, { 1, 0 }, 1000, 500);
        CHECK(d.out[0] == 1397);
        CHECK(d.out[1] == 1697);
    }

    // Shift 13 to 15 acts as shift 12 with nibble >> 3, so only the sign is left.
    for (int shift : { 13, 14, 15 })
    {
        const auto d = decodeBrr(shift, 0, { 7, 3, -1, -8, 0 });
        CHECK(d.out[0] == 0);
        CHECK(d.out[1] == 0);
        CHECK(d.out[2] == -2048);
        CHECK(d.out[3] == -2048);
        CHECK(d.out[4] == 0);
    }

    // 15-bit wrap. 14336 + 16383 + (-16383 >> 4 = -1024) = 29695, which wraps to -3073.
    CHECK(decodeBrr(12, 1, { 7 }, 16383, 0).out[0] == -3073);
    // -16384 - 16384 + (16384 >> 4 = 1024) = -31744, which loses its sign and wraps to +1024.
    CHECK(decodeBrr(12, 1, { -8 }, -16384, 0).out[0] == 1024);
    // The 16-bit clamp comes first: +60926 clamps to +7FFFh, which wraps to -1...
    CHECK(decodeBrr(12, 2, { 7 }, 16383, -16384).out[0] == -1);
    // ...and -62976 clamps to -8000h, which wraps to zero.
    CHECK(decodeBrr(12, 2, { -8 }, -16384, 16383).out[0] == 0);

    // The history carries the wrapped values into the next block.
    {
        const auto d = decodeBrr(4, 0, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2 });
        CHECK(d.old == 16);
        CHECK(d.older == 8);
    }

    // The flags don't change the decoding, and bits 7-4 and 3-2 are shift and filter.
    {
        const auto plain = decodeBrr(4, 1, { 3 }, 500, 0, 0);
        const auto flagged = decodeBrr(4, 1, { 3 }, 500, 0, 3);
        CHECK(plain.out[0] == flagged.out[0]);
    }
}

TEST(brr_interpolates_and_saturates)
{
    // Each tap is rounded down on its own: 361 + 2548 + 1095 + 0 = 4004, then >> 1.
    {
        const std::array<std::int16_t, 4> window { 1000, 2000, 3000, 4000 };
        CHECK(brr::interpolate(window.data(), 0) == 2002);
    }
    // i = 0x80 reads 0x038, 0x3C5, 0x3C9, 0x03A: -55 - 1885 + 2838 + 226 = 1124, then >> 1.
    {
        const std::array<std::int16_t, 4> window { -1000, -2000, 3000, 4000 };
        CHECK(brr::interpolate(window.data(), 0x80) == 562);
    }

    // The deliberate difference from the chip: the sums saturate, so full scale never flips sign.
    const std::array<std::int16_t, 4> lowest { -16384, -16384, -16384, -16384 };
    const std::array<std::int16_t, 4> highest { 16383, 16383, 16383, 16383 };
    for (int i = 0; i < 256; ++i)
    {
        const int low = brr::interpolate(lowest.data(), i);
        const int high = brr::interpolate(highest.data(), i);
        CHECK(low >= -16384 && low <= -16360);
        CHECK(high <= 16383 && high >= 16360);
    }
}

TEST(snes_interpolation_index_is_bits_4_to_11)
{
    // A slow square: note 12 reads a source sample every 16 chip samples, so each one sweeps the
    // index. The right bits give one smooth edge, and the wrong ones a saw on it at every sample.
    std::vector<int> samples(128, 4 * 2048);
    std::fill(samples.begin() + 64, samples.end(), -4 * 2048);
    const auto bank = bankOf(brrFromSamples(samples, 0));
    auto engine = makeSnes(bank, {}, 192000.0);
    engine->noteOn(12, 127);
    render(*engine, 192000 / 10);
    const auto out = render(*engine, 192000 / 2);
    const double level = peak(out);
    CHECK(level > 0.05);

    double worstRise = 0.0;
    bool falling = false;
    for (size_t i = 1; i < out.size(); ++i)
    {
        if (out[i] > 0.9 * level)
            falling = true;
        else if (out[i] < -0.9 * level)
            falling = false;
        else if (falling)
            worstRise = std::max(worstRise, static_cast<double>(out[i]) - out[i - 1]);
    }
    std::printf("  largest rise on a falling edge: %.4f of the level\n", worstRise / level);
    CHECK(worstRise < 0.001 * level);
}

TEST(snes_pitch_across_keyboard)
{
    const auto bank = bankOf(loopedSine());
    for (int note : { 36, 60, 84, 108 })
    {
        auto engine = makeSnes(bank);
        engine->noteOn(note, 127);
        const auto out = render(*engine, 96000);
        const double cents = centsBetween(measureFrequency(out, 48000.0, 4800), loopedSineFrequency(note));
        std::printf("  note %d: %+.2f cents\n", note, cents);
        CHECK(std::abs(cents) < 10.0);
    }
}

TEST(envelope_follows_rate_table)
{
    constexpr std::array<int, 32> table { 0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
                                          64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1 };
    for (size_t i = 0; i < table.size(); ++i)
        CHECK(snes::kEnvelopeRatePeriods[i] == table[i]);

    snes::EnvelopeSettings settings;
    snes::SnesEnvelope envelope;

    // Attack N = 15: rate 31, two steps of +1024.
    settings.attack = 15;
    settings.decay = 7;
    settings.sustainLevel = 7;
    settings.sustainRate = 0;
    envelope.start();
    envelope.step(settings);
    CHECK(envelope.level() == 1024);
    envelope.step(settings);
    CHECK(envelope.level() == 0x7FF);

    // Every attack rate takes its first step after the table's period.
    for (int n = 0; n < 16; ++n)
    {
        settings.attack = n;
        envelope.start();
        const int period = table[static_cast<size_t>(2 * n + 1)];
        for (int i = 0; i < period - 1; ++i)
            envelope.step(settings);
        CHECK(envelope.level() == 0);
        envelope.step(settings);
        CHECK(envelope.level() == (n == 15 ? 1024 : 32));
    }

    // Attack N = 0: rate 1, 4.1 s, and the switch to decay at 0x7E0.
    settings.attack = 0;
    envelope.start();
    int samples = 0;
    while (envelope.phase() == snes::SnesEnvelope::Phase::Attack && samples < 400000)
    {
        envelope.step(settings);
        ++samples;
    }
    std::printf("  attack N=0: %.3f s\n", samples / 32000.0);
    CHECK_NEAR(samples / 32000.0, 4.1, 0.41);
    CHECK(envelope.level() == 0x7E0);

    // Decay N = 7: rate 30. The 37 ms of the chip's table is the exponential curve from full
    // down to 1/20, which takes sustain at the same rate past the lowest boundary.
    settings.attack = 15;
    settings.decay = 7;
    settings.sustainLevel = 0;
    settings.sustainRate = 30;
    envelope.start();
    envelope.step(settings);
    envelope.step(settings);
    samples = 0;
    while (envelope.level() > 2047 / 20 && samples < 32000)
    {
        envelope.step(settings);
        ++samples;
    }
    std::printf("  decay N=7, full to 1/20: %.1f ms\n", samples / 32.0);
    CHECK_NEAR(samples / 32.0, 37.0, 37.0 * 0.25);

    // The decay itself ends on the boundary, one step at most below it: 8/8 skips it, and the
    // lowest level takes the decay's formula 880 samples, which is 27.5 ms.
    for (int level = 0; level < 8; ++level)
    {
        settings.sustainLevel = level;
        settings.sustainRate = 0;
        envelope.start();
        envelope.step(settings);
        envelope.step(settings);
        samples = 0;
        while (envelope.phase() != snes::SnesEnvelope::Phase::Sustain && samples < 32000)
        {
            envelope.step(settings);
            ++samples;
        }
        const int boundary = (level + 1) * 0x100;
        const int settled = envelope.level();
        CHECK(settled <= boundary);
        CHECK(settled > boundary - 16);
        if (level == 0)
        {
            std::printf("  decay N=7, full to the 1/8 boundary: %.1f ms\n", samples / 32.0);
            CHECK(std::abs(samples - 880) <= 4);
        }
        if (level == 7)
            CHECK(settled == 0x7FF);

        // Sustain rate 0 holds, for ever as far as 10 s can tell.
        for (int i = 0; i < 320000; ++i)
            envelope.step(settings);
        CHECK(envelope.level() == settled);
        CHECK(!envelope.finished());
    }

    // Sustain rate 31 reaches 0 within 100 ms, and that finishes the note.
    settings.sustainLevel = 7;
    settings.sustainRate = 31;
    envelope.start();
    samples = 0;
    while (!envelope.finished() && samples < 32000)
    {
        envelope.step(settings);
        ++samples;
    }
    std::printf("  sustain rate 31 to zero: %.1f ms\n", samples / 32.0);
    CHECK(envelope.finished());
    CHECK(envelope.level() == 0);
    CHECK(samples / 32.0 < 100.0);

    // The same steps reach the voices.
    const auto bank = bankOf(loopedSine());
    const auto heldLevel = [&](SnesParams params, int skip)
    {
        auto engine = makeSnes(bank, params);
        engine->noteOn(60, 127);
        render(*engine, skip);
        return rms(render(*engine, 4800));
    };
    SnesParams params;
    const double full = heldLevel(params, 4800);
    CHECK(full > 0.05);

    params.attack = 0; // halfway through its 4.03 s, and after the end
    CHECK_NEAR(heldLevel(params, 48000 * 2 - 2400), 0.5 * full, 0.1 * full);
    CHECK_NEAR(heldLevel(params, 48000 * 9 / 2), 0x7E0 / 2047.0 * full, 0.05 * full);

    params = SnesParams {};
    params.sustainLevel = 0; // settles near 1/8
    CHECK_NEAR(heldLevel(params, 4800), full / 8.0, 0.1 * full / 8.0);

    params = SnesParams {};
    params.sustainRate = 31;
    auto ending = makeSnes(bank, params);
    ending->noteOn(60, 127);
    render(*ending, 4800); // 100 ms
    CHECK(ending->activeVoices() == 0);
    CHECK(peak(render(*ending, 4800)) < 1e-6);
}

TEST(release_follows_setting)
{
    const auto bank = bankOf(loopedSine());
    for (float releaseMs : { 8.0f, 500.0f })
    {
        SnesParams params;
        params.releaseMs = releaseMs;
        auto engine = makeSnes(bank, params);
        engine->noteOn(60, 127);
        const double held = rms(render(*engine, 4800));
        engine->noteOff(60);

        constexpr int block = 16;
        int rendered = 0;
        std::vector<float> tail;
        while (engine->isNoteActive(60) && rendered < 48000 * 3)
        {
            const auto out = render(*engine, block);
            tail.insert(tail.end(), out.begin(), out.end());
            rendered += block;
        }
        const double ms = rendered / 48.0;
        std::printf("  %.0f ms release: voice ends after %.2f ms\n", static_cast<double>(releaseMs), ms);
        CHECK_NEAR(ms, releaseMs, 0.25 * releaseMs);

        // After the resampler's delay the output is silent.
        render(*engine, engine->latencySamples() + Resampler::kTaps);
        CHECK(rms(render(*engine, 480)) < 1e-6);

        // The release is linear: half the level halfway through.
        if (releaseMs >= 100.0f)
        {
            const size_t middle = tail.size() / 2;
            const std::vector<float> around(tail.begin() + static_cast<long>(middle) - 1200, tail.begin() + static_cast<long>(middle) + 1200);
            CHECK_NEAR(rms(around), 0.5 * held, 0.1 * held);
        }
    }

    // Out-of-range settings stay within what the parameter allows.
    SnesParams wild;
    wild.releaseMs = 0.0f;
    auto engine = makeSnes(bank, wild);
    engine->noteOn(60, 127);
    render(*engine, 4800);
    engine->noteOff(60);
    CHECK(engine->isNoteActive(60));
    render(*engine, 48 * 12);
    CHECK(!engine->isNoteActive(60));
}

TEST(sample_without_loop_ends_voice)
{
    // 4 blocks, 2 ms at 32 kHz.
    const auto bank = bankOf(constantSample(4, 4));
    auto engine = makeSnes(bank);
    engine->noteOn(60, 127);
    CHECK(engine->isNoteActive(60));
    render(*engine, 24); // 0.5 ms
    CHECK(engine->isNoteActive(60));
    const auto out = render(*engine, 240);
    CHECK(peak(out) > 0.01);
    CHECK(!engine->isNoteActive(60));
    render(*engine, 480);
    CHECK(rms(render(*engine, 480)) < 1e-6);

    // The end flag marks the last block, even with more data after it, and code 2 is not an end.
    std::vector<std::uint8_t> flagged;
    appendBrrBlock(flagged, 12, 0, 2, constantNibbles(4));
    appendBrrBlock(flagged, 12, 0, 1, constantNibbles(4));
    for (int i = 0; i < 50; ++i)
        appendBrrBlock(flagged, 12, 0, 0, constantNibbles(4));
    const auto flaggedBank = bankOf(SnesSample { flagged, -1, 60, "flagged" });
    auto early = makeSnes(flaggedBank);
    early->noteOn(60, 127);
    render(*early, 24);
    CHECK(early->isNoteActive(60)); // still in the first 2 blocks, 1 ms
    render(*early, 96);
    CHECK(!early->isNoteActive(60));

    // Running out of data without an end flag ends the voice too.
    std::vector<std::uint8_t> unflagged;
    appendBrrBlock(unflagged, 12, 0, 0, constantNibbles(4));
    const auto unflaggedBank = bankOf(SnesSample { unflagged, -1, 60, "unflagged" });
    auto missing = makeSnes(unflaggedBank);
    missing->noteOn(60, 127);
    render(*missing, 96);
    CHECK(!missing->isNoteActive(60));

    // A note so high that it passes whole blocks per sample stops at the end, not past it.
    auto high = makeSnes(bank);
    high->noteOn(127, 127);
    render(*high, 48);
    CHECK(!high->isNoteActive(127));

    // snesLoop only speaks for the user's sample: a bank sample has its own loopBlock.
    SnesParams looping;
    looping.loop = true;
    auto bankIgnoresLoop = makeSnes(bank, looping);
    bankIgnoresLoop->noteOn(60, 127);
    render(*bankIgnoresLoop, 480);
    CHECK(!bankIgnoresLoop->isNoteActive(60));

    const auto user = constantSample(4, 4);
    SnesParams userParams;
    userParams.instrument = SnesEngine::kUserInstrument;
    auto userOnce = makeSnes({}, userParams);
    userOnce->setUserSample(&user);
    userOnce->noteOn(60, 127);
    render(*userOnce, 480);
    CHECK(!userOnce->isNoteActive(60));

    userParams.loop = true;
    auto userLooped = makeSnes({}, userParams);
    userLooped->setUserSample(&user);
    userLooped->noteOn(60, 127);
    render(*userLooped, 48000);
    CHECK(userLooped->isNoteActive(60));

    // Turning snesLoop off lets the sample run to its end.
    userParams.loop = false;
    userLooped->setParams(userParams);
    CHECK(userLooped->isNoteActive(60));
    render(*userLooped, 480);
    CHECK(!userLooped->isNoteActive(60));
}

TEST(snes_short_user_sample_keeps_its_level_before_the_fade)
{
    const auto play = [](const SnesSample& sample, int samples)
    {
        SnesParams params;
        params.instrument = SnesEngine::kUserInstrument;
        auto engine = makeSnes({}, params, SnesEngine::kChipRate);
        engine->setUserSample(&sample);
        engine->noteOn(60, 127);
        const auto out = render(*engine, samples);
        const auto latency = static_cast<size_t>(engine->latencySamples());
        return std::vector<float>(out.begin() + static_cast<std::ptrdiff_t>(latency), out.end());
    };

    const auto longSample = constantSample(4, 100);
    const auto shortSample = constantSample(4, 4); // 64 samples: the fade takes its last quarter
    const double level = std::abs(play(longSample, 1200)[800]);
    const auto shortOut = play(shortSample, 400);
    CHECK_NEAR(std::abs(shortOut[40]), level, 0.05 * level);
    CHECK_NEAR(std::abs(shortOut[56]), 0.5 * level, 0.15 * level);
    CHECK(std::abs(shortOut[63]) < 0.2 * level);
}

TEST(snes_user_sample_fades_over_its_last_5_ms)
{
    constexpr int kLength = 100 * brr::kSamplesPerBlock;
    const auto constant = constantSample(4, 100);

    SnesParams params;
    params.instrument = SnesEngine::kUserInstrument;
    const auto play = [&](const SnesEngine::Bank& bank, const SnesParams& settings)
    {
        auto engine = makeSnes(bank, settings, SnesEngine::kChipRate); // one output sample per sample of the chip
        engine->setUserSample(&constant);
        engine->noteOn(60, 127);
        const auto out = render(*engine, kLength + 1000);
        const auto latency = static_cast<size_t>(engine->latencySamples());
        std::vector<double> levels;
        for (size_t i = latency; i < out.size(); ++i)
            levels.push_back(std::abs(static_cast<double>(out[i])));
        return levels;
    };

    // Without a loop: untouched up to the last 160 samples, then down in a straight line to nothing.
    const auto once = play({}, params);
    const double level = once[800];
    CHECK(level > 0.05);
    CHECK_NEAR(once[kLength - 170], level, 0.01 * level);
    CHECK_NEAR(once[kLength - 120], 0.75 * level, 0.03 * level);
    CHECK_NEAR(once[kLength - 80], 0.5 * level, 0.03 * level);
    CHECK_NEAR(once[kLength - 40], 0.25 * level, 0.03 * level);
    CHECK(once[kLength - 1] < 0.03 * level);
    CHECK(peak(std::vector<float>(once.begin() + kLength + 40, once.end())) < 1e-6);

    // With a loop there is no end to fade towards.
    auto looping = params;
    looping.loop = true;
    const auto looped = play({}, looping);
    double lowest = level;
    for (int i = kLength - 200; i < kLength + 200; ++i)
        lowest = std::min(lowest, looped[static_cast<size_t>(i)]);
    CHECK(lowest > 0.9 * level);

    // A bank instrument makes its own ending.
    const auto bank = bankOf(constant);
    SnesParams bankParams;
    const auto banked = play(bank, bankParams);
    CHECK_NEAR(banked[kLength - 40], level, 0.01 * level);
}

TEST(snes_loop_returns_to_loop_block)
{
    // Two positive blocks, then two negative ones: a loop on block 2 stays negative.
    std::vector<std::uint8_t> brr;
    appendBrrBlock(brr, 12, 0, 0, constantNibbles(4));
    appendBrrBlock(brr, 12, 0, 0, constantNibbles(4));
    appendBrrBlock(brr, 12, 0, 0, constantNibbles(-4));
    appendBrrBlock(brr, 12, 0, 3, constantNibbles(-4));
    const SnesSample sample { brr, 2, 60, "loop" };
    const auto bank = bankOf(sample);
    auto engine = makeSnes(bank);
    engine->noteOn(60, 127);
    const auto first = render(*engine, 48); // 1 ms: the positive part
    double highest = 0.0;
    for (auto s : first)
        highest = std::max(highest, static_cast<double>(s));
    CHECK(highest > 0.05);

    render(*engine, 96);
    auto after = render(*engine, 48000);
    double lowest = 0.0;
    highest = -1.0;
    for (auto s : after)
    {
        lowest = std::min(lowest, static_cast<double>(s));
        highest = std::max(highest, static_cast<double>(s));
    }
    CHECK(engine->isNoteActive(60));
    CHECK(highest < -0.05);
    CHECK(lowest > -0.25);

    // Without a valid loopBlock the user's sample starts over, so both parts keep coming back.
    SnesSample user = sample;
    user.loopBlock = -1;
    SnesParams params;
    params.instrument = SnesEngine::kUserInstrument;
    params.loop = true;
    auto restarting = makeSnes({}, params);
    restarting->setUserSample(&user);
    restarting->noteOn(60, 127);
    render(*restarting, 4800);
    after = render(*restarting, 4800);
    lowest = highest = 0.0;
    for (auto s : after)
    {
        lowest = std::min(lowest, static_cast<double>(s));
        highest = std::max(highest, static_cast<double>(s));
    }
    CHECK(highest > 0.05);
    CHECK(lowest < -0.05);

    // A loopBlock past the end of a bank sample means no loop.
    SnesSample broken = sample;
    broken.loopBlock = 99;
    const auto brokenBank = bankOf(broken);
    auto ends = makeSnes(brokenBank);
    ends->noteOn(60, 127);
    render(*ends, 480);
    CHECK(!ends->isNoteActive(60));
}

TEST(snes_ninth_note_steals_oldest)
{
    const auto bank = bankOf(loopedSine());
    auto engine = makeSnes(bank);
    for (int note = 60; note < 69; ++note)
    {
        engine->noteOn(note, 100);
        render(*engine, 100);
    }
    CHECK(engine->activeVoices() == 8);
    CHECK(!engine->isNoteActive(60));
    CHECK(engine->isNoteActive(61));
    CHECK(engine->isNoteActive(68));

    // A voice that has finished is taken before the oldest held one.
    auto free = makeSnes(bank);
    for (int note = 60; note < 68; ++note)
        free->noteOn(note, 100);
    free->noteOff(63);
    render(*free, 4800);
    CHECK(free->activeVoices() == 7);
    free->noteOn(70, 100);
    CHECK(free->activeVoices() == 8);
    CHECK(free->isNoteActive(60));
    CHECK(free->isNoteActive(70));
    CHECK(!free->isNoteActive(63));

    // A held note reuses its voice.
    auto held = makeSnes(bank);
    held->noteOn(60, 100);
    held->noteOn(60, 100);
    CHECK(held->activeVoices() == 1);

    // All 8 sound: the chord is louder than one note.
    auto one = makeSnes(bank);
    one->noteOn(60, 127);
    const double single = rms(render(*one, 48000), 4800);
    auto chord = makeSnes(bank);
    for (int note = 60; note < 68; ++note)
        chord->noteOn(note, 127);
    CHECK(rms(render(*chord, 48000), 4800) > 2.0 * single);
}

TEST(snes_sustain_and_bend)
{
    const auto bank = bankOf(loopedSine());
    auto engine = makeSnes(bank);
    engine->setSustain(true);
    engine->noteOn(60, 100);
    const double held = rms(render(*engine, 4800));
    engine->noteOff(60);
    render(*engine, 48000);
    CHECK(engine->isNoteActive(60));
    CHECK_NEAR(rms(render(*engine, 4800)), held, 0.1 * held);
    engine->setSustain(false);
    render(*engine, 4800);
    CHECK(!engine->isNoteActive(60));
    CHECK(rms(render(*engine, 480)) < 1e-6);

    // Lifting the pedal leaves keys that are physically down.
    engine->setSustain(true);
    engine->noteOn(62, 100);
    engine->setSustain(false);
    render(*engine, 4800);
    CHECK(engine->isNoteActive(62));
    engine->allNotesOff();

    auto bent = makeSnes(bank);
    bent->noteOn(69, 127);
    render(*bent, 4800);
    bent->setPitchBend(2.0);
    render(*bent, 4800);
    const double up = centsBetween(measureFrequency(render(*bent, 48000), 48000.0), loopedSineFrequency(71));
    bent->setPitchBend(-2.0);
    render(*bent, 4800);
    const double down = centsBetween(measureFrequency(render(*bent, 48000), 48000.0), loopedSineFrequency(67));
    std::printf("  bend up %+.2f cents, down %+.2f cents\n", up, down);
    CHECK(std::abs(up) < 10.0);
    CHECK(std::abs(down) < 10.0);
}

TEST(snes_vibrato_and_glide_move_pitch)
{
    const auto bank = bankOf(loopedSine());
    const auto spread = [&](float depth)
    {
        SnesParams params;
        params.vibratoDepthCents = depth;
        params.vibratoRateHz = 5.0f;
        auto engine = makeSnes(bank, params);
        engine->noteOn(69, 127);
        render(*engine, 2400);
        double low = 1e9, high = 0.0;
        for (int i = 0; i < 25; ++i)
        {
            const double f = measureFrequency(render(*engine, 2400), 48000.0);
            low = std::min(low, f);
            high = std::max(high, f);
        }
        return high - low;
    };
    CHECK(spread(0.0f) < 2.0);
    CHECK(spread(100.0f) > 0.08 * loopedSineFrequency(69)); // the full swing is about 12 %, windows average it

    SnesParams params;
    params.glideMs = 1000.0f;
    auto engine = makeSnes(bank, params);
    engine->noteOn(57, 127);
    engine->noteOff(57);
    render(*engine, 4800);
    engine->noteOn(69, 127);
    const double early = measureFrequency(render(*engine, 4800), 48000.0);
    render(*engine, 48000);
    const double late = measureFrequency(render(*engine, 9600), 48000.0);
    CHECK(early < 0.65 * loopedSineFrequency(69));
    CHECK(std::abs(centsBetween(late, loopedSineFrequency(69))) < 10.0);
}

TEST(snes_root_note_and_velocity)
{
    // A bank sample's own root note counts, not the parameter.
    auto rooted = loopedSine();
    rooted.rootNote = 48;
    const auto rootedBank = bankOf(rooted);
    SnesParams params;
    params.rootNote = 72;
    auto bankEngine = makeSnes(rootedBank, params);
    bankEngine->noteOn(60, 127);
    CHECK(std::abs(centsBetween(measureFrequency(render(*bankEngine, 48000), 48000.0, 4800), 2.0 * loopedSineFrequency(60))) < 10.0);

    // The user's sample takes the parameter, even during a note.
    SnesParams userParams;
    userParams.instrument = SnesEngine::kUserInstrument;
    userParams.rootNote = 72;
    userParams.loop = true;
    auto userEngine = makeSnes({}, userParams);
    userEngine->setUserSample(&rooted);
    userEngine->noteOn(60, 127);
    CHECK(std::abs(centsBetween(measureFrequency(render(*userEngine, 48000), 48000.0, 4800), 0.5 * loopedSineFrequency(60))) < 10.0);
    userParams.rootNote = 60;
    userEngine->setParams(userParams);
    render(*userEngine, 2400);
    CHECK(userEngine->isNoteActive(60));
    CHECK(std::abs(centsBetween(measureFrequency(render(*userEngine, 48000), 48000.0), loopedSineFrequency(60))) < 10.0);

    // Velocity scales the voice when it is on.
    const auto bank = bankOf(loopedSine());
    const auto level = [&](bool velocityOn, int velocity)
    {
        SnesParams p;
        p.velocityOn = velocityOn;
        auto engine = makeSnes(bank, p);
        engine->noteOn(60, velocity);
        return rms(render(*engine, 48000), 4800);
    };
    const double loud = level(true, 127);
    CHECK_NEAR(level(true, 64), loud * 64.0 / 127.0, 0.01 * loud);
    CHECK_NEAR(level(false, 64), loud, 0.01 * loud);
}

TEST(snes_output_level_comparable)
{
    const auto bank = bankOf(loopedSine());
    auto engine = makeSnes(bank);
    engine->noteOn(69, 127);
    const double snesLevel = rms(render(*engine, 48000), 4800);

    auto pulse = makeEngine(NesChannel::Pulse);
    pulse.noteOn(69, 127);
    const double pulseLevel = rms(render(pulse, 48000));
    std::printf("  sine %.4f, 2A03 pulse %.4f, %+.2f dB\n", snesLevel, pulseLevel, decibels(snesLevel / pulseLevel));
    CHECK(std::abs(decibels(snesLevel / pulseLevel)) < 6.0);
}

TEST(snes_silent_without_sample)
{
    const auto silent = [](SnesEngine& engine)
    {
        engine.noteOn(60, 127);
        const bool quiet = peak(render(engine, 2400)) == 0.0;
        return quiet && engine.activeVoices() == 0 && !engine.isNoteActive(60);
    };

    // No bank at all, or no user's sample.
    SnesParams params;
    CHECK(silent(*makeSnes({}, params)));
    params.instrument = SnesEngine::kUserInstrument;
    CHECK(silent(*makeSnes({}, params)));

    // An index past a short bank, a missing entry, and a sample with no full block.
    const auto bank = bankOf(loopedSine());
    params.instrument = 5;
    CHECK(silent(*makeSnes(bank, params)));

    SnesEngine::Bank holes { nullptr };
    params.instrument = 0;
    CHECK(silent(*makeSnes(holes, params)));

    SnesEngine::Bank empty { std::make_shared<const SnesSample>(SnesSample { std::vector<std::uint8_t>(8, 0), -1, 60, "short" }) };
    CHECK(silent(*makeSnes(empty, params)));

    // Importing a sample then gives sound to the next note.
    params.instrument = SnesEngine::kUserInstrument;
    const auto user = loopedSine();
    auto engine = makeSnes({}, params);
    engine->noteOn(60, 127);
    engine->setUserSample(&user);
    CHECK(engine->activeVoices() == 0);
    engine->noteOn(60, 127);
    CHECK(engine->activeVoices() == 1);
    CHECK(rms(render(*engine, 2400)) > 0.01);
}

TEST(snes_new_user_sample_stops_its_voices)
{
    const auto first = loopedSine();
    const auto second = loopedSine();
    SnesParams params;
    params.instrument = SnesEngine::kUserInstrument;
    params.loop = true;
    auto engine = makeSnes({}, params);
    engine->setUserSample(&first);
    engine->noteOn(60, 127);
    engine->noteOn(64, 127);
    render(*engine, 480);
    CHECK(engine->activeVoices() == 2);

    // The same pointer changes nothing.
    engine->setUserSample(&first);
    CHECK(engine->activeVoices() == 2);

    engine->setUserSample(&second);
    CHECK(engine->activeVoices() == 0);
    render(*engine, engine->latencySamples() + Resampler::kTaps);
    CHECK(rms(render(*engine, 480)) < 1e-6);
    engine->noteOn(60, 127);
    CHECK(rms(render(*engine, 2400)) > 0.01);

    // Taking the sample away silences it for good.
    engine->setUserSample(nullptr);
    CHECK(engine->activeVoices() == 0);
    engine->noteOn(60, 127);
    CHECK(engine->activeVoices() == 0);

    // A voice on the bank keeps playing through an import, and a new bank stops it.
    const auto bank = bankOf(loopedSine());
    const auto otherBank = bankOf(loopedSine());
    auto bankEngine = makeSnes(bank);
    bankEngine->noteOn(60, 127);
    bankEngine->setUserSample(&first);
    CHECK(bankEngine->activeVoices() == 1);
    bankEngine->setBank(&bank);
    CHECK(bankEngine->activeVoices() == 1);
    bankEngine->setBank(&otherBank);
    CHECK(bankEngine->activeVoices() == 0);
}

TEST(snes_instrument_change_stops_notes)
{
    const SnesEngine::Bank bank { std::make_shared<const SnesSample>(loopedSine()), std::make_shared<const SnesSample>(loopedSine()) };
    SnesParams params;
    auto engine = makeSnes(bank, params);
    engine->noteOn(60, 127);
    render(*engine, 480);

    // Other parameters leave notes alone.
    params.attack = 3;
    params.releaseMs = 100.0f;
    params.rootNote = 40;
    engine->setParams(params);
    CHECK(engine->activeVoices() == 1);

    params.instrument = 1;
    engine->setParams(params);
    CHECK(engine->activeVoices() == 0);
    CHECK(peak(render(*engine, 480)) == 0.0);
    engine->noteOn(60, 127);
    CHECK(engine->activeVoices() == 1);
}

TEST(snes_all_notes_off_leaves_no_tail)
{
    const auto bank = bankOf(loopedSine());
    auto engine = makeSnes(bank);
    engine->setSustain(true);
    engine->noteOn(60, 127);
    engine->noteOn(64, 127);
    render(*engine, 2400);
    engine->allNotesOff();
    CHECK(engine->activeVoices() == 0);

    // Not even what the resampler still held.
    CHECK(peak(render(*engine, 480)) == 0.0);
    engine->setSustain(false);

    // The next note plays exactly as on a fresh engine, with no stale glide or filter state.
    SnesParams gliding;
    gliding.glideMs = 200.0f;
    auto used = makeSnes(bank, gliding);
    used->noteOn(40, 127);
    render(*used, 1234);
    used->allNotesOff();
    used->noteOn(60, 127);
    auto fresh = makeSnes(bank, gliding);
    fresh->noteOn(60, 127);
    const auto usedOut = render(*used, 4800);
    const auto freshOut = render(*fresh, 4800);
    double worst = 0.0;
    for (size_t i = 0; i < usedOut.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(usedOut[i]) - freshOut[i]));
    CHECK(worst == 0.0);
    CHECK(rms(usedOut) > 0.01);
}

TEST(snes_invalid_events_are_ignored)
{
    const auto bank = bankOf(loopedSine());
    auto engine = makeSnes(bank);
    engine->noteOn(-1, 100);
    engine->noteOn(128, 100);
    engine->noteOff(60);
    CHECK(engine->activeVoices() == 0);
    engine->noteOn(60, 100);
    engine->noteOn(60, 0); // velocity 0 is a note-off
    CHECK(engine->isNoteActive(60));
    engine->allNotesOff();

    SnesParams wild;
    wild.instrument = -5;
    wild.attack = 99;
    wild.decay = -1;
    wild.sustainLevel = 99;
    wild.sustainRate = 99;
    wild.releaseMs = 1e9f;
    wild.rootNote = 999;
    engine->setParams(wild);
    engine->noteOn(60, 100);
    CHECK(engine->isNoteActive(60));
    CHECK(allFinite(render(*engine, 2400)));
    CHECK(!engine->isNoteActive(60)); // the sustain rate clamps to 31, which ends the note

    // A stray bend is out of tune with everything, but must not break anything.
    auto bent = makeSnes(bank);
    bent->noteOn(60, 100);
    bent->setPitchBend(1e6);
    CHECK(allFinite(render(*bent, 2400)));
    bent->setPitchBend(-1e6);
    CHECK(allFinite(render(*bent, 2400)));
    CHECK(bent->isNoteActive(60));
}

TEST(snes_finite_at_all_rates)
{
    const auto bank = bankOf(loopedSine());
    for (double sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        auto engine = makeSnes(bank, {}, sampleRate);
        for (int note : { 0, 40, 69, 127 })
            engine->noteOn(note, 127);
        engine->setPitchBend(2.0);
        const auto out = render(*engine, 24000);
        CHECK(allFinite(out));
        CHECK(peak(out) > 0.0);
        CHECK(engine->latencySamples() > 0);

        auto tuned = makeSnes(bank, {}, sampleRate);
        tuned->noteOn(69, 127);
        const auto held = render(*tuned, static_cast<int>(sampleRate));
        CHECK(std::abs(centsBetween(measureFrequency(held, sampleRate, 4800), loopedSineFrequency(69))) < 10.0);
    }
}

TEST(snes_large_and_empty_blocks)
{
    const auto bank = bankOf(loopedSine());
    for (double sampleRate : { 44100.0, 192000.0 })
    {
        auto whole = makeSnes(bank, {}, sampleRate);
        auto split = makeSnes(bank, {}, sampleRate);
        whole->noteOn(69, 127);
        split->noteOn(69, 127);

        // Far more than one resampler chunk, and more than its queue holds.
        constexpr int total = 200000;
        const auto wholeOut = render(*whole, total);
        std::vector<float> splitOut(total, 0.0f);
        split->render(splitOut.data(), 0);
        for (int done = 0, block = 1; done < total; block = block % 1000 + 37)
        {
            const int n = std::min(block, total - done);
            split->render(splitOut.data() + done, n);
            done += n;
        }

        double worst = 0.0;
        for (size_t i = 0; i < wholeOut.size(); ++i)
            worst = std::max(worst, std::abs(static_cast<double>(wholeOut[i]) - splitOut[i]));
        CHECK(worst < 1e-6);
        CHECK(std::abs(centsBetween(measureFrequency(wholeOut, sampleRate, 2000), loopedSineFrequency(69))) < 10.0);
    }
}

TEST(snes_silent_before_prepare)
{
    const auto bank = bankOf(loopedSine());
    SnesEngine engine;
    engine.setBank(&bank);
    engine.noteOn(69, 127);
    CHECK(engine.activeVoices() == 0);
    std::vector<float> out(512, 1.0f);
    engine.render(out.data(), 512);
    CHECK(peak(out) == 0.0);
}

TEST(snes_prepare_resets_voices)
{
    const auto bank = bankOf(loopedSine());
    auto engine = makeSnes(bank, {}, 44100.0);
    engine->noteOn(69, 127);
    render(*engine, 1000);
    engine->prepare(96000.0);
    CHECK(engine->activeVoices() == 0);
    CHECK(peak(render(*engine, 960)) == 0.0);
    engine->noteOn(69, 127);
    const auto out = render(*engine, 192000);
    CHECK(std::abs(centsBetween(measureFrequency(out, 96000.0, 9600), loopedSineFrequency(69))) < 10.0);
}

TEST(snes_audio_thread_calls_do_not_allocate)
{
    const auto bank = bankOf(loopedSine());
    const auto otherBank = bankOf(loopedSine());
    const auto user = loopedSine();
    const auto otherUser = loopedSine();

    SnesParams params;
    params.vibratoDepthCents = 30.0f;
    params.glideMs = 100.0f;
    auto engine = makeSnes(bank, params);
    auto changed = params;
    changed.attack = 8;
    changed.decay = 3;
    changed.sustainRate = 12;
    changed.releaseMs = 50.0f;
    auto toUser = changed;
    toUser.instrument = SnesEngine::kUserInstrument;
    toUser.loop = true;
    std::vector<float> block(30000);

    const auto before = allocationCount.load();
    for (int note = 50; note < 62; ++note)
    {
        engine->noteOn(note, 100);
        engine->render(block.data(), 64);
    }
    engine->setParams(changed);
    engine->setPitchBend(1.5);
    engine->render(block.data(), 30000); // beyond one resampler chunk
    engine->setSustain(true);
    engine->noteOff(61);
    engine->setSustain(false);
    engine->noteOff(60);
    engine->render(block.data(), 512);
    engine->setBank(&otherBank);
    engine->setParams(toUser);
    engine->setUserSample(&user);
    engine->noteOn(70, 90);
    engine->render(block.data(), 512);
    engine->setUserSample(&otherUser);
    engine->noteOn(72, 90);
    engine->render(block.data(), 512);
    engine->allNotesOff();
    engine->render(block.data(), 512);
    const auto after = allocationCount.load();

    CHECK(after == before);
}

// ---- Task 6: echo, encoder, bank -------------------------------------------------------------

namespace
{
constexpr double kChipRate = SnesEngine::kChipRate;

// Decodes a whole BRR stream in order, as the chip does on its first pass. old and older carry
// on through the blocks, so a looped stream can be decoded for a second pass by passing them on.
std::vector<int> decodeStream(const std::vector<std::uint8_t>& bytes, int* oldOut = nullptr, int* olderOut = nullptr)
{
    std::vector<int> out;
    int old = oldOut != nullptr ? *oldOut : 0;
    int older = olderOut != nullptr ? *olderOut : 0;
    for (size_t b = 0; b + brr::kBytesPerBlock <= bytes.size(); b += brr::kBytesPerBlock)
    {
        std::int16_t block[brr::kSamplesPerBlock];
        brr::decodeBlock(bytes.data() + b, old, older, block);
        out.insert(out.end(), block, block + brr::kSamplesPerBlock);
    }
    if (oldOut != nullptr)
        *oldOut = old;
    if (olderOut != nullptr)
        *olderOut = older;
    return out;
}

// Deterministic noise in -1 to +1, without touching any global generator.
struct TestNoise
{
    std::uint32_t state;
    explicit TestNoise(std::uint32_t seed) : state(seed) {}
    float next()
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<double>(state >> 8) / 8388608.0 - 1.0);
    }
};

std::vector<float> sinePcm(size_t n, double frequency, double amplitude)
{
    std::vector<float> pcm(n);
    for (size_t i = 0; i < n; ++i)
        pcm[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / kChipRate));
    return pcm;
}

bool inBrrRange(const std::vector<int>& decoded)
{
    return std::all_of(decoded.begin(), decoded.end(), [](int v) { return v >= -0x3FFA && v <= 0x3FF8; });
}

// Error of the decoded stream against the input (scaled so that 1.0 is 0x3FF8), relative to the
// input, in dB.
double roundTripErrorDb(const std::vector<float>& pcm, const std::vector<int>& decoded)
{
    double signal = 0.0, error = 0.0;
    for (size_t i = 0; i < pcm.size(); ++i)
    {
        const double e = decoded[i] / 16376.0 - pcm[i];
        signal += static_cast<double>(pcm[i]) * pcm[i];
        error += e * e;
    }
    return decibels(std::sqrt(error / signal));
}

// Runs an echo in uneven chunks, to show that chunking changes nothing.
void runEcho(SnesEcho& echo, std::vector<float>& x, float mix, float feedback)
{
    size_t done = 0;
    for (int chunk = 1; done < x.size(); chunk = chunk % 211 + 13)
    {
        const size_t n = std::min(static_cast<size_t>(chunk), x.size() - done);
        echo.process(x.data() + done, static_cast<int>(n), mix, feedback);
        done += n;
    }
}

size_t indexOfPeak(const std::vector<float>& x, size_t from, size_t to)
{
    size_t best = from;
    for (size_t i = from; i < std::min(to, x.size()); ++i)
        if (std::abs(x[i]) > std::abs(x[best]))
            best = i;
    return best;
}

double centroid(const std::vector<float>& x, size_t from, size_t to)
{
    double weighted = 0.0, total = 0.0;
    for (size_t i = from; i < std::min(to, x.size()); ++i)
    {
        weighted += static_cast<double>(i) * x[i];
        total += x[i];
    }
    return weighted / total;
}
} // namespace

TEST(echo_delay_matches_setting)
{
    for (int steps : { 1, 4, 9, 15 })
    {
        const size_t delay = static_cast<size_t>(steps) * 512;
        SnesEcho echo;
        echo.setDelay(steps);
        std::vector<float> x(delay + 1500, 0.0f);
        x[0] = 1.0f;
        runEcho(echo, x, 1.0f, 0.0f);

        // The dry impulse stays, and the filtered one comes back at the set delay.
        CHECK_NEAR(x[0], 1.0, 1e-6);
        const size_t at = indexOfPeak(x, 1, x.size());
        std::printf("  %d steps: peak at %zu, set %zu, centroid %.2f, level %.5f\n", steps, at, delay, centroid(x, 1, x.size()), x[at]);
        CHECK(std::abs(static_cast<long>(at) - static_cast<long>(delay)) <= 2);
        CHECK_NEAR(centroid(x, 1, x.size()), static_cast<double>(delay), 2.0);
        CHECK_NEAR(x[at], 23.0 / 128.0, 1e-6); // the filter's largest coefficient

        double total = 0.0;
        for (size_t i = 1; i < x.size(); ++i)
            total += x[i];
        CHECK_NEAR(total, 127.0 / 128.0, 1e-5); // the coefficients' sum
        CHECK(std::abs(x[delay / 2]) == 0.0);   // and nothing before it
    }

    // The filter is the chip's, oldest tap first.
    SnesEcho echo;
    echo.setDelay(1);
    std::vector<float> x(1200, 0.0f);
    x[0] = 1.0f;
    runEcho(echo, x, 1.0f, 0.0f);
    const std::array<int, 8> taps { 4, 10, 16, 20, 23, 22, 18, 14 };
    const size_t first = indexOfPeak(x, 1, x.size()) - 3; // the 23 is the fifth tap, three samples after the first
    for (size_t k = 0; k < taps.size(); ++k)
        CHECK_NEAR(x[first + 7 - k] * 128.0, taps[k], 1e-3);

    // The feedback brings it back again one delay later.
    SnesEcho looped;
    looped.setDelay(1);
    std::vector<float> y(1600, 0.0f);
    y[0] = 1.0f;
    runEcho(looped, y, 0.0f, 1.0f); // mix 0 leaves the output dry
    CHECK_NEAR(peak(std::vector<float>(y.begin() + 1, y.end())), 0.0, 1e-9);
    SnesEcho heard;
    heard.setDelay(1);
    std::vector<float> z(1600, 0.0f);
    z[0] = 1.0f;
    runEcho(heard, z, 1.0f, 1.0f);
    const size_t second = indexOfPeak(z, 700, 1400);
    std::printf("  second repeat: peak at %zu, centroid %.2f\n", second, centroid(z, 700, 1500));
    CHECK_NEAR(centroid(z, 700, 1500), 1024.0, 3.0);
}

TEST(echo_stays_stable_at_full_feedback)
{
    SnesEcho echo;
    echo.setDelay(4);
    constexpr size_t seconds = 60;
    std::vector<float> x(static_cast<size_t>(kChipRate), 0.0f);
    double earlyPeak = 0.0, latePeak = 0.0, lastSecond = 0.0;
    bool finite = true;
    for (size_t second = 0; second < seconds; ++second)
    {
        std::fill(x.begin(), x.end(), 0.0f);
        if (second == 0)
            x[0] = 1.0f;
        echo.process(x.data(), static_cast<int>(x.size()), 1.0f, 1.0f);
        finite = finite && allFinite(x);
        std::vector<float> echoes(x.begin() + (second == 0 ? 1 : 0), x.end());
        if (second == 0)
            earlyPeak = peak(echoes);
        if (second == seconds - 1)
            lastSecond = peak(echoes);
        if (second >= 10 && second < 11)
            latePeak = peak(echoes);
    }
    std::printf("  peak after 0 s %.5f, 10 s %.6f, 59 s %.8f\n", earlyPeak, latePeak, lastSecond);
    CHECK(finite);
    CHECK(earlyPeak > 0.0);
    CHECK(latePeak < earlyPeak); // it only dies away
    CHECK(lastSecond < latePeak);
    CHECK(lastSecond < 0.1 * earlyPeak);

    // A held loud input settles at a finite level too, the gain at DC being 127/128 per pass.
    SnesEcho held;
    held.setDelay(15);
    std::vector<float> dc(static_cast<size_t>(kChipRate), 0.5f);
    for (int second = 0; second < 30; ++second)
    {
        std::fill(dc.begin(), dc.end(), 0.5f);
        held.process(dc.data(), static_cast<int>(dc.size()), 1.0f, 1.0f);
        CHECK(allFinite(dc));
    }
    CHECK(peak(dc) < 0.5 * 128.0 + 1e-3); // bounded by the geometric series, 0.5 / (1 - 127/128)

    // Settings beyond 100 % are held at 100 %, not allowed to run away.
    SnesEcho wild;
    wild.setDelay(1);
    std::vector<float> w(static_cast<size_t>(kChipRate) * 5, 0.0f);
    w[0] = 1.0f;
    wild.process(w.data(), static_cast<int>(w.size()), 50.0f, 50.0f);
    CHECK(allFinite(w));
    CHECK(peak(w) <= 1.0 + 1e-6);
}

TEST(echo_zero_delay_is_dry)
{
    SnesEcho echo;
    echo.setDelay(0);
    TestNoise noise(7);
    std::vector<float> x(5000);
    for (auto& s : x)
        s = noise.next();
    const auto dry = x;
    echo.process(x.data(), static_cast<int>(x.size()), 1.0f, 1.0f);
    CHECK(x == dry);

    // Going to 0 from a running echo leaves no tail either.
    SnesEcho running;
    running.setDelay(3);
    auto y = dry;
    running.process(y.data(), static_cast<int>(y.size()), 1.0f, 0.9f);
    CHECK(y != dry);
    running.setDelay(0);
    y = dry;
    running.process(y.data(), static_cast<int>(y.size()), 1.0f, 0.9f);
    CHECK(y == dry);

    // Out-of-range delays are held in range.
    SnesEcho clamped;
    clamped.setDelay(-3);
    y = dry;
    clamped.process(y.data(), static_cast<int>(y.size()), 1.0f, 1.0f);
    CHECK(y == dry);
    clamped.setDelay(1000);
    y.assign(20000, 0.0f);
    y[0] = 1.0f;
    clamped.process(y.data(), static_cast<int>(y.size()), 1.0f, 0.0f);
    CHECK(indexOfPeak(y, 1, y.size()) == static_cast<size_t>(SnesEcho::kMaxSteps * SnesEcho::kStepSamples));

    // A mix of 0 is dry too, while the line keeps running.
    SnesEcho silent;
    silent.setDelay(2);
    y = dry;
    silent.process(y.data(), static_cast<int>(y.size()), 0.0f, 0.5f);
    CHECK(y == dry);
}

TEST(echo_delay_change_does_not_replay_stale_data)
{
    // Fill the whole line with noise, shorten the delay and let the short line empty out.
    SnesEcho echo;
    echo.setDelay(15);
    TestNoise noise(11);
    std::vector<float> x(20000);
    for (auto& s : x)
        s = noise.next();
    echo.process(x.data(), static_cast<int>(x.size()), 1.0f, 0.5f);

    echo.setDelay(1);
    std::vector<float> quiet(3000, 0.0f);
    echo.process(quiet.data(), static_cast<int>(quiet.size()), 1.0f, 0.0f);

    // Lengthening it must not bring back what was in the line before, only silence.
    echo.setDelay(15);
    std::vector<float> after(10000, 0.0f);
    echo.process(after.data(), static_cast<int>(after.size()), 1.0f, 0.5f);
    CHECK(peak(after) == 0.0);

    // The same holds for each step on the way up, and from a bypassed echo.
    SnesEcho stepped;
    stepped.setDelay(2);
    stepped.process(x.data(), static_cast<int>(x.size()), 1.0f, 0.5f);
    stepped.setDelay(0);
    stepped.process(quiet.data(), static_cast<int>(quiet.size()), 1.0f, 0.5f);
    std::vector<float> tail(12000, 0.0f);
    for (int steps = 1; steps <= 15; ++steps)
    {
        std::fill(tail.begin(), tail.end(), 0.0f);
        stepped.setDelay(steps);
        stepped.process(tail.data(), 700, 1.0f, 0.5f);
        CHECK(peak(tail) == 0.0);
    }

    // What the line does hold is kept when the delay grows: a note played before still comes back.
    SnesEcho kept;
    kept.setDelay(1);
    std::vector<float> impulse(300, 0.0f);
    impulse[0] = 1.0f;
    kept.process(impulse.data(), static_cast<int>(impulse.size()), 1.0f, 0.0f);
    kept.setDelay(2); // the impulse is 300 samples old, and in the line
    std::vector<float> later(2000, 0.0f);
    kept.process(later.data(), static_cast<int>(later.size()), 1.0f, 0.0f);
    // It comes out once, at the new delay counted from where it went in (1024), and not at the old one (512).
    CHECK(peak(std::vector<float>(later.begin(), later.begin() + 700)) == 0.0);
    CHECK(std::abs(static_cast<long>(indexOfPeak(later, 0, later.size())) + 300 - 1024) <= 2);
}

TEST(echo_is_cleared_when_notes_stop_by_console_change)
{
    const auto bank = bankOf(loopedSine());
    SnesParams params;
    params.echoMix = 1.0f;
    params.echoDelay = 2;
    params.echoFeedback = 1.0f;
    auto engine = makeSnes(bank, params);
    engine->noteOn(60, 127);
    engine->noteOn(64, 127);
    const auto playing = render(*engine, 24000);
    const double level = peak(playing);
    CHECK(level > 0.05);

    // The echo is running by now, so it would ring on for seconds.
    engine->allNotesOff();
    const auto stopped = render(*engine, 960); // 20 ms at 48 kHz
    CHECK(decibels(peak(stopped) / level) < -60.0);
    const auto later = render(*engine, 96000);
    CHECK(peak(later) == 0.0);
    CHECK(engine->activeVoices() == 0);

    // The next note starts clean, exactly as on a new engine.
    engine->allNotesOff();
    engine->noteOn(60, 127);
    auto fresh = makeSnes(bank, params);
    fresh->noteOn(60, 127);
    const auto usedOut = render(*engine, 9600);
    const auto freshOut = render(*fresh, 9600);
    double worst = 0.0;
    for (size_t i = 0; i < usedOut.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(usedOut[i]) - freshOut[i]));
    CHECK(worst == 0.0);
    CHECK(rms(usedOut) > 0.01);

    // An instrument change stops the notes the same way.
    const SnesEngine::Bank two { bank[0], bank[0] };
    auto changing = makeSnes(two, params);
    changing->noteOn(60, 127);
    render(*changing, 24000);
    auto next = params;
    next.instrument = 1;
    changing->setParams(next);
    render(*changing, 960);
    CHECK(peak(render(*changing, 48000)) == 0.0);
}

TEST(snes_echo_reaches_the_output)
{
    const auto bank = bankOf(loopedSine());
    const auto burst = [&](float mix, int delay)
    {
        SnesParams params;
        params.echoMix = mix;
        params.echoDelay = delay;
        params.echoFeedback = 0.0f;
        auto engine = makeSnes(bank, params);
        engine->noteOn(60, 127);
        render(*engine, 480);
        engine->noteOff(60);
        render(*engine, 2400); // the note has ended (8 ms release) and the resampler has drained
        return render(*engine, 48000);
    };
    // 4 steps are 64 ms: the echo is heard after the note (which ended 60 ms in), and not without the mix.
    const auto dry = burst(0.0f, 4);
    const auto wet = burst(1.0f, 4);
    CHECK(peak(dry) < 1e-6);
    CHECK(peak(wet) > 0.01);
    // At 5 steps (80 ms) it arrives earlier than at 12 (192 ms).
    const auto firstAbove = [](const std::vector<float>& x)
    {
        for (size_t i = 0; i < x.size(); ++i)
            if (std::abs(x[i]) > 0.002)
                return i;
        return x.size();
    };
    const auto near = firstAbove(burst(1.0f, 5));
    const auto far = firstAbove(burst(1.0f, 12));
    CHECK(near < far);
    CHECK(far - near > 4000);
}

TEST(encoder_round_trip_error)
{
    constexpr size_t n = 32000;

    // A sine at -6 dB is what the plan's bound is for.
    const auto sine = sinePcm(n, 440.0, 0.5);
    const auto sineBytes = encodeBrr(sine, -1);
    CHECK(sineBytes.size() == n / brr::kSamplesPerBlock * brr::kBytesPerBlock);
    const auto sineDecoded = decodeStream(sineBytes);
    const double sineDb = roundTripErrorDb(sine, sineDecoded);
    std::printf("  sine: error %.1f dB, %zu bytes\n", sineDb, sineBytes.size());
    CHECK(sineDb < -30.0);

    // A few more pitches and levels, since a good result at one frequency says little.
    for (double frequency : { 100.0, 1000.0, 3000.0 })
    {
        const auto tone = sinePcm(n, frequency, 0.5);
        const auto db = roundTripErrorDb(tone, decodeStream(encodeBrr(tone, -1)));
        std::printf("  sine %.0f Hz: error %.1f dB\n", frequency, db);
        CHECK(db < -30.0);
    }

    // Noise has no structure to predict, so only the format limits it: finite and in range.
    TestNoise generator(3);
    std::vector<float> noise(n);
    for (auto& s : noise)
        s = 0.5f * generator.next();
    const auto noiseDecoded = decodeStream(encodeBrr(noise, -1));
    CHECK(noiseDecoded.size() == n);
    CHECK(inBrrRange(noiseDecoded));
    const double noiseDb = roundTripErrorDb(noise, noiseDecoded);
    std::printf("  noise: error %.1f dB\n", noiseDb);
    CHECK(std::isfinite(noiseDb));
    CHECK(noiseDb < -10.0); // not a requirement of the format: a guard against a broken search

    // The input is not changed, and an encode is repeatable.
    CHECK(encodeBrr(sine, -1) == sineBytes);
}

TEST(encoder_respects_range)
{
    TestNoise generator(5);
    std::vector<std::vector<float>> inputs;
    inputs.push_back(sinePcm(8000, 200.0, 1.0));               // full scale
    inputs.push_back(sinePcm(8000, 200.0, 3.0));               // far beyond it
    inputs.push_back(sinePcm(8000, 7000.0, 1.0));
    std::vector<float> square(4000), impulses(4000, 0.0f), noise(8000), steps(4000);
    for (size_t i = 0; i < square.size(); ++i)
    {
        square[i] = (i / 37) % 2 == 0 ? 1.0f : -1.0f;
        steps[i] = (i / 100) % 2 == 0 ? 0.999f : -0.999f;
    }
    for (size_t i = 0; i < impulses.size(); i += 101)
        impulses[i] = i % 2 == 0 ? 1.0f : -1.0f;
    for (auto& s : noise)
        s = 2.0f * generator.next();
    inputs.push_back(square);
    inputs.push_back(impulses);
    inputs.push_back(noise);
    inputs.push_back(steps);
    inputs.push_back(std::vector<float>(2000, 1.0f));
    inputs.push_back(std::vector<float>(2000, -1.0f));
    inputs.push_back({ std::nanf(""), INFINITY, -INFINITY, 0.5f });

    for (const auto& pcm : inputs)
    {
        const auto decoded = decodeStream(encodeBrr(pcm, -1));
        CHECK(decoded.size() >= pcm.size());
        CHECK(inBrrRange(decoded));
    }

    // Nothing is lost in the clamping: full scale reaches the top of the range.
    const auto full = decodeStream(encodeBrr(std::vector<float>(160, 1.0f), -1));
    CHECK(*std::max_element(full.begin(), full.end()) >= 0x3FF8 - 2048);
    const auto low = decodeStream(encodeBrr(std::vector<float>(160, -1.0f), -1));
    CHECK(*std::min_element(low.begin(), low.end()) <= -0x3FFA + 2048);
}

TEST(encoder_framing_and_flags)
{
    // Nothing in, nothing out.
    CHECK(encodeBrr({}, -1).empty());
    CHECK(encodeBrr({}, 0).empty());

    // Short input is padded with zeros to a whole block.
    const auto five = encodeBrr(std::vector<float>(5, 0.5f), -1);
    CHECK(five.size() == 9);
    CHECK((five[0] & 3) == 1); // the end flag, and not the loop flag
    const auto padded = decodeStream(five);
    CHECK(padded.size() == 16);
    double squares = 0.0; // against 8188 for the five samples, and 0 for the padding
    for (size_t i = 0; i < 16; ++i)
        squares += std::pow(padded[i] - (i < 5 ? 8188.0 : 0.0), 2.0);
    CHECK(std::sqrt(squares / 16.0) < 600.0);

    // Whole blocks, the end flag on the last one only.
    const auto pcm = sinePcm(16 * 10 + 3, 300.0, 0.4);
    const auto bytes = encodeBrr(pcm, -1);
    CHECK(bytes.size() == 11 * 9);
    for (size_t b = 0; b < 11; ++b)
        CHECK((bytes[b * 9] & 3) == (b == 10 ? 1 : 0));

    // With a loop point, the loop flag goes on the last block as well.
    const auto looped = encodeBrr(pcm, 32);
    CHECK(looped.size() == bytes.size());
    for (size_t b = 0; b < 11; ++b)
        CHECK((looped[b * 9] & 3) == (b == 10 ? 3 : 0));
    CHECK(encodeBrr(pcm, 0)[10 * 9] % 4 == 3);

    // A loop point is rounded up to a block, and one past the end is no loop.
    CHECK(brrLoopBlockFor(-1) == -1);
    CHECK(brrLoopBlockFor(0) == 0);
    CHECK(brrLoopBlockFor(1) == 1);
    CHECK(brrLoopBlockFor(16) == 1);
    CHECK(brrLoopBlockFor(17) == 2);
    CHECK(brrLoopBlockFor(2147483647) > 0);
    CHECK((encodeBrr(pcm, 100000)[10 * 9] & 3) == 1);
}

TEST(encoder_loop_block_uses_filter_0)
{
    // A smooth tone invites the higher filters everywhere, so a filter 0 block stands out.
    const auto pcm = sinePcm(16 * 40, 150.0, 0.6);
    const auto plain = encodeBrr(pcm, -1);
    int predicted = 0;
    for (size_t b = 1; b < 40; ++b)
        predicted += ((plain[b * 9] >> 2) & 3) != 0 ? 1 : 0;
    CHECK(predicted > 20); // the search does use filters 1 to 3 when it may

    for (int loopBlock : { 0, 1, 7, 20, 39 })
    {
        const auto looped = encodeBrr(pcm, loopBlock * 16);
        CHECK(((looped[0] >> 2) & 3) == 0); // the first block, which has no history, also
        CHECK(((looped[static_cast<size_t>(loopBlock) * 9] >> 2) & 3) == 0);
        CHECK(loopBlock == 0 || looped.size() == plain.size());
    }

    // An unaligned loop point rounds up, and that block is the one with filter 0.
    const auto rounded = encodeBrr(pcm, 16 * 12 + 1);
    CHECK(((rounded[13 * 9] >> 2) & 3) == 0);

    // What the filter-0 block is for: the decoder, coming back from the end, plays it the same
    // as the first time, whatever history it carries.
    const auto looped = encodeBrr(pcm, 16 * 12);
    int old = 0, older = 0;
    const auto firstPass = decodeStream(looped, &old, &older);
    const std::vector<std::uint8_t> loopPart(looped.begin() + 12 * 9, looped.end());
    const auto secondPass = decodeStream(loopPart, &old, &older);
    for (size_t i = 0; i < secondPass.size(); ++i)
        CHECK(secondPass[i] == firstPass[12 * 16 + i]);
}

namespace
{
struct BankFacts
{
    std::string name;
    size_t bytes = 0;
    int root = 0;
    int loopSamples = 0; // 0 for none
    double rms = 0.0;
    double pitchCents = 0.0;
};

// The note the fundamental falls on: the loop's own cycles when there is one, otherwise a scan of
// the decoded start. Returns the error in cents against the equal-tempered root.
double rootPitchErrorCents(const SnesSample& sample, const std::vector<int>& decoded)
{
    const double expected = 440.0 * std::pow(2.0, (sample.rootNote - 69) / 12.0);
    const bool looped = sample.loopBlock >= 0;
    const size_t start = looped ? static_cast<size_t>(sample.loopBlock) * 16 : 1000;
    const size_t length = looped ? decoded.size() - start : std::min<size_t>(decoded.size() - start, 16384);

    const auto magnitude = [&](double frequency)
    {
        // A Hann window for the scan, none for a loop, where whole cycles make bins exact.
        const double w = 2.0 * kPi * frequency / kChipRate;
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < length; ++i)
        {
            const double window = looped ? 1.0 : 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(length));
            const double v = decoded[start + i] * window;
            re += v * std::cos(w * static_cast<double>(i));
            im += v * std::sin(w * static_cast<double>(i));
        }
        return re * re + im * im;
    };

    if (looped)
    {
        const double binHz = kChipRate / static_cast<double>(length);
        const int expectedBin = static_cast<int>(std::lround(expected / binHz));
        int bestBin = expectedBin;
        double best = 0.0;
        for (int bin = std::max(1, expectedBin - 4); bin <= expectedBin + 4; ++bin)
        {
            const double m = magnitude(bin * binHz);
            if (m > best)
            {
                best = m;
                bestBin = bin;
            }
        }
        return centsBetween(bestBin * binHz, expected);
    }

    double best = 0.0, bestCents = 0.0;
    for (double cents = -60.0; cents <= 60.0; cents += 0.25)
    {
        const double m = magnitude(expected * std::pow(2.0, cents / 1200.0));
        if (m > best)
        {
            best = m;
            bestCents = cents;
        }
    }
    return bestCents;
}

BankFacts factsAbout(const SnesSample& sample)
{
    const auto decoded = decodeStream(sample.brr);
    BankFacts facts;
    facts.name = sample.name;
    facts.bytes = sample.brr.size();
    facts.root = sample.rootNote;
    const size_t from = sample.loopBlock >= 0 ? static_cast<size_t>(sample.loopBlock) * 16 : 0;
    facts.loopSamples = sample.loopBlock >= 0 ? static_cast<int>(decoded.size() - from) : 0;
    double sum = 0.0;
    for (size_t i = from; i < decoded.size(); ++i)
        sum += static_cast<double>(decoded[i]) * decoded[i];
    facts.rms = std::sqrt(sum / static_cast<double>(decoded.size() - from)) / 16376.0;
    facts.pitchCents = rootPitchErrorCents(sample, decoded);
    return facts;
}

const std::array<const char*, 12> kBankNames { "Strings", "Brass", "Organ", "Bass", "Bell", "Flute", "Choir", "Piano", "Guitar", "Marimba", "Reed", "Pad" };
// The instruments that sound as long as a key is held, and so must loop.
constexpr std::array<bool, 12> kBankSustained { true, true, true, true, false, true, true, false, false, false, true, true };
} // namespace

TEST(bank_has_12_distinct_looping_instruments)
{
    const auto bank = buildSnesBank();
    const auto again = buildSnesBank();
    CHECK(bank.size() == 12);
    CHECK(again.size() == 12);
    if (bank.size() != 12 || again.size() != 12)
        return;

    for (size_t i = 0; i < bank.size(); ++i)
    {
        const auto& s = *bank[i];
        CHECK(s.name == kBankNames[i]);

        // Built twice, the same to the byte.
        CHECK(s.brr == again[i]->brr);
        CHECK(s.loopBlock == again[i]->loopBlock);
        CHECK(s.rootNote == again[i]->rootNote);
        CHECK(s.name == again[i]->name);

        // A valid stream within the limit: whole blocks, the end flag only on the last, and the
        // loop flag exactly when there is a loop point inside the sample.
        const size_t blocks = s.brr.size() / brr::kBytesPerBlock;
        CHECK(s.brr.size() % brr::kBytesPerBlock == 0);
        CHECK(blocks >= 16);
        CHECK(s.brr.size() <= 64 * 1024);
        CHECK(s.rootNote >= 24 && s.rootNote <= 96);
        for (size_t b = 0; b < blocks; ++b)
            CHECK(brr::hasEndFlag(s.brr[b * brr::kBytesPerBlock]) == (b + 1 == blocks));
        const bool loopFlag = (s.brr[(blocks - 1) * brr::kBytesPerBlock] & 2) != 0;
        CHECK(loopFlag == (s.loopBlock >= 0));
        CHECK(s.loopBlock < static_cast<int>(blocks));
        if (kBankSustained[i])
            CHECK(s.loopBlock >= 0);
        if (s.loopBlock >= 0)
        {
            CHECK(((s.brr[static_cast<size_t>(s.loopBlock) * brr::kBytesPerBlock] >> 2) & 3) == 0);
            CHECK((blocks - static_cast<size_t>(s.loopBlock)) * brr::kSamplesPerBlock >= 256);
        }
        CHECK(((s.brr[0] >> 2) & 3) == 0);
        CHECK(inBrrRange(decodeStream(s.brr)));
    }

    // All different, in bytes and in sound.
    for (size_t i = 0; i < bank.size(); ++i)
    {
        const auto a = decodeStream(bank[i]->brr);
        for (size_t j = i + 1; j < bank.size(); ++j)
        {
            CHECK(bank[i]->brr != bank[j]->brr);
            CHECK(bank[i]->name != bank[j]->name);
            const auto b = decodeStream(bank[j]->brr);
            const size_t m = std::min<size_t>({ a.size(), b.size(), 8000 });
            double ab = 0.0, aa = 0.0, bb = 0.0;
            for (size_t k = 0; k < m; ++k)
            {
                ab += static_cast<double>(a[k]) * b[k];
                aa += static_cast<double>(a[k]) * a[k];
                bb += static_cast<double>(b[k]) * b[k];
            }
            const double correlation = ab / std::sqrt(aa * bb);
            if (std::abs(correlation) > 0.9)
                std::printf("  %s and %s correlate at %.3f\n", bank[i]->name.c_str(), bank[j]->name.c_str(), correlation);
            CHECK(std::abs(correlation) < 0.9);
        }
    }

    // Each one plays at its root, through the engine and its envelope.
    for (size_t i = 0; i < bank.size(); ++i)
    {
        SnesParams params;
        params.instrument = static_cast<int>(i);
        auto engine = makeSnes(bank, params);
        engine->noteOn(bank[i]->rootNote, 127);
        const auto out = render(*engine, 24000);
        CHECK(allFinite(out));
        CHECK(rms(out, 2400) > 0.01);
        if (rms(out, 2400) <= 0.01)
            std::printf("  %s is too quiet\n", bank[i]->name.c_str());

        // The sustained ones are still going after seconds, as loud as at the start.
        if (kBankSustained[i])
        {
            const auto long_ = render(*engine, 48000 * 6);
            CHECK(engine->activeVoices() == 1);
            const std::vector<float> lastSecond(long_.end() - 48000, long_.end());
            CHECK(rms(lastSecond) > 0.5 * rms(out, 2400));
            CHECK(rms(lastSecond) < 2.0 * rms(out, 2400));
        }
    }
}

TEST(bank_loops_are_seamless)
{
    const auto bank = buildSnesBank();
    for (const auto& sample : bank)
    {
        if (sample->loopBlock < 0)
            continue;

        // After the first pass the loop repeats exactly, whatever the filters carry over.
        const size_t from = static_cast<size_t>(sample->loopBlock) * brr::kBytesPerBlock;
        int old = 0, older = 0;
        const auto first = decodeStream(sample->brr, &old, &older);
        const std::vector<std::uint8_t> loopPart(sample->brr.begin() + static_cast<long>(from), sample->brr.end());
        const auto second = decodeStream(loopPart, &old, &older);
        const size_t start = static_cast<size_t>(sample->loopBlock) * brr::kSamplesPerBlock;
        bool same = second.size() == first.size() - start;
        for (size_t i = 0; same && i < second.size(); ++i)
            same = second[i] == first[start + i];
        CHECK(same);

        // The step across the seam is no bigger than the steps inside the loop.
        int largest = 0;
        for (size_t i = start + 1; i < first.size(); ++i)
            largest = std::max(largest, std::abs(first[i] - first[i - 1]));
        const int seam = std::abs(first[start] - first.back());
        std::printf("  %-8s seam step %5d, largest step in loop %5d\n", sample->name.c_str(), seam, largest);
        CHECK(seam <= largest + largest / 4 + 64);
    }
}

TEST(bank_report_sizes_levels_and_pitch)
{
    const auto bank = buildSnesBank();
    size_t total = 0;
    for (const auto& sample : bank)
    {
        const auto facts = factsAbout(*sample);
        total += facts.bytes;
        std::printf("  %-8s %6zu bytes, root %3d, loop %6d samples, rms %.3f, pitch at root %+.2f cents\n", facts.name.c_str(), facts.bytes,
                    facts.root, facts.loopSamples, facts.rms, facts.pitchCents);
        CHECK(facts.bytes <= 64 * 1024);
        CHECK(facts.rms > 0.02);
        CHECK(facts.loopSamples % 16 == 0);
        CHECK(std::abs(facts.pitchCents) < 2.0);
    }
    std::printf("  bank: %zu bytes in all\n", total);
}

TEST(snes_8_voices_echo_under_25_percent_cpu)
{
    const auto bank = buildSnesBank();
    for (double sampleRate : { 44100.0, 192000.0 })
    {
        SnesParams params;
        params.instrument = 0;
        params.echoMix = 1.0f; // 100 %
        params.echoDelay = 15;
        params.echoFeedback = 1.0f;
        params.vibratoDepthCents = 20.0f;
        auto engine = makeSnes(bank, params, sampleRate);
        for (int note = 60; note < 68; ++note)
            engine->noteOn(note, 127);

        std::vector<float> block(512);
        const auto start = std::chrono::steady_clock::now();
        constexpr int seconds = 10;
        const int blocks = static_cast<int>(seconds * sampleRate / 512);
        for (int i = 0; i < blocks; ++i)
            engine->render(block.data(), 512);
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        std::printf("  %.0f Hz: CPU %.1f %% of one core\n", sampleRate, 100.0 * elapsed.count() / seconds);
        CHECK(peak(block) > 0.01);
        CHECK(engine->activeVoices() == 8);
        CHECK(allFinite(block));
        CHECK(elapsed.count() < 0.25 * seconds);
    }
}


// ---- SoundFont bank and engine --------------------------------------------------------------

namespace
{
using testsf2::TestSf2Options;

std::unique_ptr<SoundFontBank> makeBank(const TestSf2Options& options = {}, double sampleRate = 48000.0)
{
    const auto file = testsf2::buildTestSf2(options);
    return SoundFontBank::fromMemory(file.data(), static_cast<int>(file.size()), sampleRate);
}

// The engine keeps a pointer to the bank, which the caller must keep alive.
std::unique_ptr<SoundFontEngine> makeSf(SoundFontBank* bank, const SoundFontParams& params = {}, double sampleRate = 48000.0)
{
    auto engine = std::make_unique<SoundFontEngine>();
    engine->setBank(bank);
    engine->setParams(params);
    engine->prepare(sampleRate);
    return engine;
}

// Both zones of the test font are in tune with the note's equal-tempered frequency.
double sfLevel(SoundFontEngine& engine, int note)
{
    engine.noteOn(note, 127);
    const double level = rms(render(engine, 24000), 4800);
    engine.allNotesOff();
    return level;
}

struct Counts
{
    long long cpp, tsf;
};

Counts allocationCounts() { return { allocationCount.load(), soundfont::allocationCount() }; }

// Plays a few notes on a bank the loader accepted, which must not fail whatever the file held.
void playEverything(SoundFontBank& bank)
{
    SoundFontEngine engine;
    engine.setBank(&bank);
    engine.prepare(48000.0);
    const int count = std::min(bank.instrumentCount(), 6);
    for (int instrument = 0; instrument < count; ++instrument)
    {
        SoundFontParams params;
        params.instrument = instrument;
        engine.setParams(params);
        for (int note : { 0, 36, 59, 60, 72, 127 })
            for (int velocity : { 30, 120 })
            {
                engine.noteOn(note, velocity);
                CHECK(allFinite(render(engine, 1200)));
                engine.noteOff(note);
            }
        CHECK(allFinite(render(engine, 4800)));
        engine.allNotesOff();
    }
}

void setU32(std::vector<std::uint8_t>& f, size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        f[at + static_cast<size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}

// Position of the first record of a pdta chunk.
size_t pdtaRecord(const std::vector<std::uint8_t>& f, const char* id)
{
    for (size_t at : testsf2::chunkHeaderOffsets(f))
        if (std::string(reinterpret_cast<const char*>(&f[at]), 4) == id)
            return at + 8;
    return 0;
}

// ---- Files built by hand, for the structure check ----

using testsf2::kAttenuation;
using testsf2::kInitialFilterFc;
using testsf2::kInstrument;
using testsf2::kKeyRange;
using testsf2::kReleaseVolEnv;
using testsf2::kSampleId;
using testsf2::kSampleModes;
using testsf2::kVelRange;
using testsf2::InstrumentSpec;
using testsf2::PresetSpec;
using testsf2::range;

// Global zones in both the preset and the instrument, as real fonts have them: a zone without a sample
// (or without an instrument) comes first and gives its settings to the zones after it. Here the
// instrument's loops and release, and the preset's attenuation.
std::vector<std::uint8_t> globalZoneFont()
{
    const InstrumentSpec instrument { "Global",
                                      { { { kSampleModes, 1 }, { kReleaseVolEnv, testsf2::timecents(0.2) } },
                                        { { kKeyRange, range(0, 59) }, { kSampleId, 0 } },
                                        { { kKeyRange, range(60, 127) }, { kSampleId, 1 } } } };
    const PresetSpec preset { "Global", 0, 0, { { { kAttenuation, 0 } }, { { kInstrument, 0 } } } };
    return testsf2::buildSf2({ preset }, { instrument });
}

// The preset's own global zone limits it to the low half; TinySoundFont counts two regions for it and
// fills one. The high half is then silent, and the unfilled region is zeros.
std::vector<std::uint8_t> underFilledFont()
{
    const InstrumentSpec instrument { "Under",
                                      { { { kKeyRange, range(0, 59) }, { kSampleModes, 1 }, { kSampleId, 0 } },
                                        { { kKeyRange, range(60, 127) }, { kSampleModes, 1 }, { kSampleId, 1 } } } };
    const PresetSpec preset { "Under", 0, 0, { { { kKeyRange, range(0, 59) } }, { { kInstrument, 0 } } } };
    return testsf2::buildSf2({ preset }, { instrument });
}

// TinySoundFont counts a preset's regions with each zone's own range, key or velocity, and fills them
// using the range the zone inherited from a global zone. Ranges above 127 are never met by the
// first count, so a global zone with one makes the fill write a region the count did not allow for.
std::vector<std::uint8_t> overflowFont(int rangeOp)
{
    const InstrumentSpec instrument { "Overflow",
                                      { { { rangeOp, range(200, 255) }, { kInitialFilterFc, 13500 } },
                                        { { kInitialFilterFc, 13500 }, { kSampleId, 1 } } } };
    const PresetSpec preset { "Overflow", 0, 0, { { { rangeOp, range(200, 255) }, { kInstrument, 0 } } } };
    return testsf2::buildSf2({ preset }, { instrument });
}

// Several presets and instruments with zones of every kind, for mutation.
std::vector<std::uint8_t> richFont()
{
    const std::vector<InstrumentSpec> instruments {
        { "A", { { { kSampleModes, 1 }, { kReleaseVolEnv, testsf2::timecents(0.2) } },
                 { { kKeyRange, range(0, 59) }, { kSampleId, 0 } },
                 { { kKeyRange, range(60, 127) }, { kSampleId, 1 } } } },
        { "B", { { { kKeyRange, range(0, 127) }, { kVelRange, range(0, 63) }, { kSampleId, 0 } },
                 { { kKeyRange, range(0, 127) }, { kVelRange, range(64, 127) }, { kSampleModes, 1 }, { kSampleId, 1 } } } },
        { "C", { { { kKeyRange, range(40, 80) }, { kInitialFilterFc, 9000 } },
                 { { kVelRange, range(0, 127) }, { kSampleId, 1 } },
                 { { kKeyRange, range(0, 20) }, { kSampleId, 0 } } } }
    };
    const std::vector<PresetSpec> presets {
        { "P0", 0, 0, { { { kAttenuation, 0 } }, { { kKeyRange, range(0, 127) }, { kInstrument, 0 } }, { { kKeyRange, range(36, 96) }, { kVelRange, range(0, 100) }, { kInstrument, 1 } } } },
        { "P1", 0, 1, { { { kInstrument, 1 } } } },
        { "P2", 1, 0, { { { kKeyRange, range(0, 59) } }, { { kInstrument, 0 } }, { { kInstrument, 2 } } } },
        { "P3", 1, 1, { { { kVelRange, range(30, 127) } }, { { kInstrument, 2 } } } }
    };
    return testsf2::buildSf2(presets, instruments);
}

// Puts bytes at a position and grows the RIFF, and any lists named by their header offsets, to match.
std::vector<std::uint8_t> insertBytes(std::vector<std::uint8_t> f, size_t at, const std::vector<std::uint8_t>& bytes, std::vector<size_t> growing = {})
{
    growing.push_back(0);
    for (size_t header : growing)
    {
        const std::uint32_t size = f[header + 4] | (f[header + 5] << 8) | (f[header + 6] << 16) | (static_cast<std::uint32_t>(f[header + 7]) << 24);
        setU32(f, header + 4, size + static_cast<std::uint32_t>(bytes.size()));
    }
    f.insert(f.begin() + static_cast<long>(at), bytes.begin(), bytes.end());
    return f;
}

// The header offset and end of the LIST of a given type.
std::pair<size_t, size_t> listNamed(const std::vector<std::uint8_t>& f, const char* type)
{
    for (size_t at : testsf2::chunkHeaderOffsets(f))
        if (std::string(reinterpret_cast<const char*>(&f[at]), 4) == "LIST" && std::string(reinterpret_cast<const char*>(&f[at + 8]), 4) == type)
        {
            const std::uint32_t size = f[at + 4] | (f[at + 5] << 8) | (f[at + 6] << 16) | (static_cast<std::uint32_t>(f[at + 7]) << 24);
            return { at, at + 8 + size };
        }
    return { 0, 0 };
}

std::vector<std::uint8_t> chunkBytes(const char* id, std::vector<std::uint8_t> body)
{
    std::vector<std::uint8_t> out(id, id + 4);
    out.resize(8);
    setU32(out, 4, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// True if the file loads and then plays a note of each zone in tune.
bool playsInTune(const std::vector<std::uint8_t>& file)
{
    const auto bank = SoundFontBank::fromMemory(file.data(), static_cast<int>(file.size()), 48000.0);
    if (!bank)
        return false;
    auto engine = makeSf(bank.get());
    for (int note : { 48, 72 })
    {
        engine->noteOn(note, 127);
        const auto out = render(*engine, 48000);
        if (std::abs(centsBetween(measureFrequency(out, 48000.0, 4800), noteFrequency(note))) > 10.0)
            return false;
        engine->allNotesOff();
    }
    return true;
}

int fuzzRounds()
{
    const char* text = std::getenv("RETROCHIP_FUZZ_ROUNDS");
    return text != nullptr ? std::max(1, std::atoi(text)) : 6000;
}

} // namespace

TEST(sf_pitch_across_keyboard)
{
    const auto bank = makeBank();
    CHECK(bank != nullptr);
    for (int note : { 48, 60, 72, 84 })
    {
        auto engine = makeSf(bank.get());
        engine->noteOn(note, 127);
        const auto out = render(*engine, 96000);
        const double cents = centsBetween(measureFrequency(out, 48000.0, 4800), noteFrequency(note));
        std::printf("  note %d: %+.2f cents\n", note, cents);
        CHECK(std::abs(cents) < 10.0);
    }

    // The zone below note 60 plays the quiet sample, and notes from 60 the loud one.
    auto engine = makeSf(bank.get());
    const double below = sfLevel(*engine, 59);
    const double from = sfLevel(*engine, 60);
    const double low = sfLevel(*engine, 48);
    const double high = sfLevel(*engine, 84);
    std::printf("  levels: 48 %.4f, 59 %.4f, 60 %.4f, 84 %.4f\n", low, below, from, high);
    CHECK_NEAR(from / below, 0.8 / 0.25, 0.05 * 0.8 / 0.25);
    CHECK_NEAR(low, below, 0.02 * below);
    CHECK_NEAR(high, from, 0.02 * from);
}

TEST(sf_loop_holds_and_release_ends)
{
    // The sample lasts 0.18 s at its root, so a level held for 2 s can only come from the loop.
    const auto bank = makeBank();
    auto engine = makeSf(bank.get());
    engine->noteOn(69, 127);
    render(*engine, 4800);
    const double first = rms(render(*engine, 12000));
    for (int window = 0; window < 7; ++window)
        CHECK_NEAR(rms(render(*engine, 12000)), first, 0.02 * first);
    CHECK(engine->activeVoices() == 1);

    // Release is 0.2 s: still sounding and falling at 0.1 s, gone by 0.35 s.
    engine->noteOff(69);
    const double early = rms(render(*engine, 4800));
    CHECK(early < first);
    CHECK(early > 0.0);
    CHECK(engine->activeVoices() == 1);
    render(*engine, 12000);
    CHECK(engine->activeVoices() == 0);
    CHECK(peak(render(*engine, 4800)) == 0.0);

    // Without a loop the same sample ends by itself.
    TestSf2Options oneShot;
    oneShot.loop = false;
    const auto shotBank = makeBank(oneShot);
    auto shot = makeSf(shotBank.get());
    shot->noteOn(69, 127);
    CHECK(rms(render(*shot, 4800)) > 0.1);
    render(*shot, 24000);
    CHECK(shot->activeVoices() == 0);
    CHECK(peak(render(*shot, 4800)) == 0.0);
}

TEST(sf_bend_sustain_velocity_and_all_off)
{
    const auto bank = makeBank();
    auto engine = makeSf(bank.get());

    // A bend moves the note that is already sounding, and the ones that follow.
    engine->noteOn(69, 127);
    render(*engine, 4800);
    engine->setPitchBend(2.0);
    render(*engine, 4800);
    const double up = centsBetween(measureFrequency(render(*engine, 48000), 48000.0), noteFrequency(71));
    engine->setPitchBend(-2.0);
    render(*engine, 4800);
    const double down = centsBetween(measureFrequency(render(*engine, 48000), 48000.0), noteFrequency(67));
    std::printf("  bend up %+.2f cents, down %+.2f cents\n", up, down);
    CHECK(std::abs(up) < 10.0);
    CHECK(std::abs(down) < 10.0);
    engine->setPitchBend(0.0);
    engine->allNotesOff();
    engine->setPitchBend(2.0);
    engine->noteOn(60, 127);
    const double fresh = centsBetween(measureFrequency(render(*engine, 48000), 48000.0, 4800), noteFrequency(62));
    CHECK(std::abs(fresh) < 10.0);
    engine->allNotesOff();

    // The pedal holds a released note, and lifting it lets the note go.
    engine->setSustain(true);
    engine->noteOn(69, 127);
    const double held = rms(render(*engine, 4800), 480);
    engine->noteOff(69);
    render(*engine, 48000);
    CHECK(engine->activeVoices() == 1);
    CHECK_NEAR(rms(render(*engine, 4800)), held, 0.02 * held);
    engine->setSustain(false);
    render(*engine, 24000);
    CHECK(engine->activeVoices() == 0);

    // Velocity scales the level, or gives one level when it is off.
    const auto levelAt = [&](bool velocityOn, int velocity)
    {
        SoundFontParams params;
        params.velocityOn = velocityOn;
        engine->setParams(params);
        engine->noteOn(69, velocity);
        const double level = rms(render(*engine, 24000), 4800);
        engine->allNotesOff();
        return level;
    };
    const double loud = levelAt(true, 127);
    CHECK_NEAR(levelAt(true, 64), loud * 64.0 / 127.0, 0.02 * loud);
    CHECK_NEAR(levelAt(false, 64), loud, 0.02 * loud);
    CHECK_NEAR(levelAt(false, 10), loud, 0.02 * loud);

    // All notes off leaves nothing, not even a tail. The pedal and the bend are the player's, and stay.
    engine->setSustain(true);
    engine->setPitchBend(2.0);
    engine->noteOn(60, 127);
    engine->noteOn(64, 127);
    render(*engine, 2400);
    CHECK(engine->activeVoices() == 2);
    engine->allNotesOff();
    CHECK(engine->activeVoices() == 0);
    CHECK(peak(render(*engine, 480)) == 0.0);
    engine->noteOn(60, 127);
    const double kept = centsBetween(measureFrequency(render(*engine, 48000), 48000.0, 4800), noteFrequency(62));
    CHECK(std::abs(kept) < 10.0);
    engine->noteOff(60);
    render(*engine, 24000);
    CHECK(engine->activeVoices() == 1); // still held by the pedal
    engine->setSustain(false);
    render(*engine, 24000);
    CHECK(engine->activeVoices() == 0);
}

TEST(sf_instrument_and_bank_change_stop_notes)
{
    TestSf2Options two;
    two.presets = { { "One", 0, 0 }, { "Two", 0, 1 } };
    const auto bank = makeBank(two);
    const auto other = makeBank(two);
    SoundFontParams params;
    auto engine = makeSf(bank.get(), params);
    engine->noteOn(60, 127);
    render(*engine, 480);

    params.velocityOn = false;
    engine->setParams(params);
    CHECK(engine->activeVoices() == 1);

    params.instrument = 1;
    engine->setParams(params);
    CHECK(engine->activeVoices() == 0);
    CHECK(peak(render(*engine, 480)) == 0.0);
    engine->noteOn(60, 127);
    CHECK(engine->activeVoices() == 1);

    engine->setBank(other.get());
    CHECK(engine->activeVoices() == 0);
    CHECK(peak(render(*engine, 480)) == 0.0);
    engine->noteOn(60, 127);
    CHECK(peak(render(*engine, 2400)) > 0.1);

    // An instrument beyond the bank's list plays the last one rather than nothing.
    params.instrument = 99;
    engine->setParams(params);
    engine->noteOn(60, 127);
    CHECK(peak(render(*engine, 2400)) > 0.1);
    params.instrument = -4;
    engine->setParams(params);
    engine->noteOn(60, 127);
    CHECK(peak(render(*engine, 2400)) > 0.1);
}

TEST(bank_lists_instruments)
{
    TestSf2Options options;
    options.presets = { { "Zeta", 1, 5 }, { "Alpha", 0, 3 }, { "Beta", 0, 1 } };
    const auto bank = makeBank(options);
    CHECK(bank != nullptr);
    CHECK(bank->instrumentCount() == 3);

    // In bank order, then program order, whatever order the file lists them in.
    CHECK(bank->instrumentName(0) == "Beta");
    CHECK(bank->instrumentName(1) == "Alpha");
    CHECK(bank->instrumentName(2) == "Zeta");
    CHECK(bank->bankNumber(0) == 0 && bank->programNumber(0) == 1);
    CHECK(bank->bankNumber(1) == 0 && bank->programNumber(1) == 3);
    CHECK(bank->bankNumber(2) == 1 && bank->programNumber(2) == 5);
    CHECK(bank->indexOf(0, 1) == 0);
    CHECK(bank->indexOf(0, 3) == 1);
    CHECK(bank->indexOf(1, 5) == 2);
}

TEST(bank_index_of_missing_instrument_is_minus_one)
{
    TestSf2Options options;
    options.presets = { { "Alpha", 0, 3 }, { "Beta", 2, 7 } };
    const auto bank = makeBank(options);
    CHECK(bank->indexOf(0, 4) == -1);
    CHECK(bank->indexOf(1, 3) == -1);
    CHECK(bank->indexOf(2, 3) == -1);
    CHECK(bank->indexOf(-1, -1) == -1);
    CHECK(bank->indexOf(99999, 99999) == -1);

    // Positions outside the list give empty answers.
    CHECK(bank->instrumentName(-1).empty());
    CHECK(bank->instrumentName(2).empty());
    CHECK(bank->bankNumber(2) == -1);
    CHECK(bank->programNumber(-1) == -1);
}

TEST(bank_survives_damaged_files)
{
    const auto file = testsf2::buildTestSf2();
    const int size = static_cast<int>(file.size());

    const auto attempt = [](const std::vector<std::uint8_t>& bytes)
    {
        const auto bank = SoundFontBank::fromMemory(bytes.data(), static_cast<int>(bytes.size()), 48000.0);
        if (bank)
            playEverything(*bank);
        return bank != nullptr;
    };

    // The intact file loads, and nothing that is not a file does.
    CHECK(attempt(file));
    CHECK(SoundFontBank::fromMemory(nullptr, 0, 48000.0) == nullptr);
    CHECK(SoundFontBank::fromMemory(file.data(), 0, 48000.0) == nullptr);
    CHECK(SoundFontBank::fromMemory(file.data(), -5, 48000.0) == nullptr);
    CHECK(!attempt(std::vector<std::uint8_t>(1000, 0x41)));
    CHECK(!attempt(std::vector<std::uint8_t>(12, 0)));

    // Cut short every 64 bytes, and just around each chunk header.
    int loadedWhenCut = 0;
    for (int length = 0; length < size; length += 64)
        loadedWhenCut += attempt(std::vector<std::uint8_t>(file.begin(), file.begin() + length));
    const auto headers = testsf2::chunkHeaderOffsets(file);
    for (size_t offset : headers)
        for (int delta : { -1, 0, 1, 4, 7, 8, 9, 12 })
        {
            const auto length = static_cast<long long>(offset) + delta;
            if (length >= 0 && length < size)
                loadedWhenCut += attempt(std::vector<std::uint8_t>(file.begin(), file.begin() + length));
        }
    std::printf("  %zu chunk headers, %d cut copies loaded\n", headers.size(), loadedWhenCut);
    CHECK(loadedWhenCut == 0); // every cut loses a needed chunk or part of one

    // Each chunk claiming to hold 4 GB, or something a little too large.
    for (size_t offset : headers)
        for (std::uint32_t claim : { 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0u, 1u, 3u })
        {
            auto copy = file;
            setU32(copy, offset + 4, claim);
            attempt(copy);
        }
    for (size_t offset : headers)
    {
        auto copy = file;
        const auto real = static_cast<std::uint32_t>(copy[offset + 4] | (copy[offset + 5] << 8) | (copy[offset + 6] << 16));
        setU32(copy, offset + 4, real + 2);
        attempt(copy);
        setU32(copy, offset + 4, real > 2 ? real - 2 : 0);
        attempt(copy);
    }

    // No presets at all: null, or a bank with nothing to play, but no crash.
    TestSf2Options none;
    none.presets.clear();
    const auto empty = makeBank(none);
    CHECK(empty == nullptr || empty->instrumentCount() == 0);

    // Sample headers pointing outside the sample data, and indexes outside their tables.
    const size_t shdr = pdtaRecord(file, "shdr");
    CHECK(shdr != 0);
    for (size_t field : { size_t(20), size_t(24), size_t(28), size_t(32), size_t(36) })
        for (std::uint32_t value : { 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 100000u, 8000u })
            for (size_t record : { size_t(0), size_t(46) })
            {
                auto copy = file;
                setU32(copy, shdr + record + field, value);
                attempt(copy);
            }
    for (const char* table : { "phdr", "pbag", "pgen", "inst", "ibag", "igen" })
    {
        const size_t at = pdtaRecord(file, table);
        for (size_t field = 20; field < 34; field += 2)
            for (std::uint8_t value : { 0xFF, 0x7F, 0x80, 0x00 })
            {
                auto copy = file;
                copy[at + field] = value;
                copy[at + field + 1] = value;
                attempt(copy);
            }
    }

    // Files that make TinySoundFont write past its region list: refused, not loaded.
    CHECK(!attempt(overflowFont(kKeyRange)));
    CHECK(!attempt(overflowFont(kVelRange)));

    // Random damage to files with global zones, ranges and several presets. The generator is fixed so a
    // failure repeats; RETROCHIP_FUZZ_ROUNDS sets the count.
    std::uint32_t state = 12345;
    const auto next = [&state]
    {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    const int rounds = fuzzRounds();
    const std::vector<std::vector<std::uint8_t>> bases { file, richFont(), globalZoneFont(), underFilledFont() };
    int flipped = 0, structured = 0, generators = 0, refused = 0;
    for (int round = 0; round < rounds; ++round)
    {
        auto copy = bases[next() % bases.size()];

        // A third of the rounds rewrite generators, a third change 16-bit fields in the tables, where the
        // indexes are, and the rest change single bytes anywhere, which rarely hit a field that matters.
        const unsigned kind = next() % 3;
        if (kind == 0)
        {
            // Whole generators in the preset and instrument zones: an operator and an amount that is
            // often a key or velocity range, with ends on either side of 127.
            static const int operators[] = { 41, 43, 44, 53, 8, 54, 43, 44 };
            static const int ends[] = { 0, 20, 59, 60, 100, 127, 128, 200, 255 };
            const int changes = 1 + static_cast<int>(next() % 5);
            for (int i = 0; i < changes; ++i)
            {
                const bool preset = next() % 2;
                const size_t at = pdtaRecord(copy, preset ? "pgen" : "igen");
                const size_t count = (static_cast<size_t>(copy[at - 4]) | (static_cast<size_t>(copy[at - 3]) << 8)) / 4;
                const size_t gen = at + (next() % count) * 4;
                const int op = operators[next() % (sizeof(operators) / sizeof(int))];
                const int amount = (op == 43 || op == 44) ? testsf2::range(ends[next() % 9], ends[next() % 9]) : static_cast<int>(next() % 3);
                copy[gen] = static_cast<std::uint8_t>(op);
                copy[gen + 1] = 0;
                copy[gen + 2] = static_cast<std::uint8_t>(amount);
                copy[gen + 3] = static_cast<std::uint8_t>(amount >> 8);
            }
            ++generators;
        }
        else if (kind == 1)
        {
            static const char* const tables[] = { "phdr", "pbag", "pgen", "inst", "ibag", "igen", "shdr" };
            static const size_t recordSizes[] = { 38, 4, 4, 22, 4, 4, 46 };
            static const int interesting[] = { 0, 1, 2, 3, 4, 5, 6, 8, 40, 41, 43, 44, 53, 54, 59, 60, 127, 128, 200, 255, 0x7F7F, 0x7FFF, 0x8000, 0xC8FF, 0xFFFF };
            const int changes = 1 + static_cast<int>(next() % 3);
            for (int i = 0; i < changes; ++i)
            {
                const size_t which = next() % 7;
                const size_t at = pdtaRecord(copy, tables[which]);
                const size_t size = (static_cast<size_t>(copy[at - 4]) | (static_cast<size_t>(copy[at - 3]) << 8)) / recordSizes[which];
                const size_t field = at + (next() % size) * recordSizes[which] + (next() % (recordSizes[which] / 2)) * 2;
                const int value = (next() % 4) ? interesting[next() % (sizeof(interesting) / sizeof(int))] : static_cast<int>(next() & 0xFFFF);
                copy[field] = static_cast<std::uint8_t>(value);
                copy[field + 1] = static_cast<std::uint8_t>(value >> 8);
            }
            ++structured;
        }
        else
        {
            const int flips = 1 + static_cast<int>(next() % 8);
            for (int i = 0; i < flips; ++i)
            {
                const size_t limit = (next() % 2) ? copy.size() : std::min<size_t>(copy.size(), 600 + static_cast<size_t>(next() % 800));
                const size_t at = (copy.size() - limit) + next() % limit;
                switch (next() % 3)
                {
                    case 0: copy[at] = static_cast<std::uint8_t>(next()); break;
                    case 1: copy[at] = 0xFF; break;
                    default: copy[at] = 0; break;
                }
            }
            ++flipped;
        }
        refused += !attempt(copy);
    }
    std::printf("  random damage: %d rounds (%d generator changes, %d field changes, %d byte changes), %d refused, %d loaded and played\n",
                rounds, generators, structured, flipped, refused, rounds - refused);
    CHECK(refused > 0);
    CHECK(refused < rounds);
}

TEST(bank_loads_global_zones)
{
    // A normal font with global zones in the preset and the instrument loads, with its settings in
    // every zone: the loop held for two seconds and the release both come from the instrument's global zone.
    const auto file = globalZoneFont();
    CHECK(playsInTune(file));
    const auto bank = SoundFontBank::fromMemory(file.data(), static_cast<int>(file.size()), 48000.0);
    auto engine = makeSf(bank.get());
    engine->noteOn(72, 127);
    render(*engine, 4800);
    const double first = rms(render(*engine, 12000));
    for (int window = 0; window < 7; ++window)
        CHECK_NEAR(rms(render(*engine, 12000)), first, 0.02 * first);
    engine->noteOff(72);
    render(*engine, 24000);
    CHECK(engine->activeVoices() == 0);

    // TinySoundFont's count is larger than its fill here: the font loads, the zone the preset leaves
    // out is silent and nothing reads the region that was never filled.
    const auto under = underFilledFont();
    const auto underBank = SoundFontBank::fromMemory(under.data(), static_cast<int>(under.size()), 48000.0);
    CHECK(underBank != nullptr);
    auto low = makeSf(underBank.get());
    low->noteOn(48, 127);
    CHECK(rms(render(*low, 24000), 4800) > 0.01);
    low->allNotesOff();
    low->noteOn(72, 127);
    CHECK(peak(render(*low, 24000)) == 0.0);
    playEverything(*underBank);
}

TEST(bank_tolerates_padding_and_unknown_chunks)
{
    const auto file = testsf2::buildTestSf2();
    CHECK(playsInTune(file));
    const auto pdta = listNamed(file, "pdta");
    const auto sdta = listNamed(file, "sdta");
    const auto info = listNamed(file, "INFO");
    CHECK(pdta.second == file.size()); // the layout these cases rely on: pdta is last

    // Four zero bytes after the last table, counted in the list and the file.
    CHECK(playsInTune(insertBytes(file, pdta.second, { 0, 0, 0, 0 }, { pdta.first })));
    CHECK(playsInTune(insertBytes(file, pdta.second, { 0 }, { pdta.first })));
    CHECK(playsInTune(insertBytes(file, pdta.second, std::vector<std::uint8_t>(7, 0x20), { pdta.first })));

    // The file size claims more than the file holds.
    for (std::uint32_t extra : { 1u, 6u, 7u, 8u, 1000u, 0x7FFFFF00u })
    {
        auto big = file;
        setU32(big, 4, static_cast<std::uint32_t>(big.size() - 8) + extra);
        CHECK(playsInTune(big));
    }

    // Bytes after the end of the file's own count.
    CHECK(playsInTune(insertBytes(file, file.size(), { 1, 2, 3, 4, 5 })));
    {
        auto tail = file;
        tail.insert(tail.end(), { 'J', 'U', 'N', 'K', 3, 0, 0, 0, 'a', 'b', 'c', 0 }); // not counted
        CHECK(playsInTune(tail));
    }

    // A trailing chunk of odd size with its pad byte, counted.
    CHECK(playsInTune(insertBytes(file, file.size(), { 'J', 'U', 'N', 'K', 3, 0, 0, 0, 'a', 'b', 'c', 0 })));

    // An odd-sized comment and its pad byte inside INFO, which is skipped whole.
    CHECK(playsInTune(insertBytes(file, info.second, { 'I', 'C', 'M', 'T', 5, 0, 0, 0, 'h', 'e', 'l', 'l', 'o', 0 }, { info.first })));

    // Chunks it does not know, between the lists and among the tables and the samples.
    const auto unknown = chunkBytes("XTRA", std::vector<std::uint8_t>(10, 7));
    CHECK(playsInTune(insertBytes(file, sdta.first, unknown)));
    CHECK(playsInTune(insertBytes(file, sdta.second, unknown, { sdta.first })));
    CHECK(playsInTune(insertBytes(file, pdtaRecord(file, "pbag") - 8, unknown, { pdta.first })));
    CHECK(playsInTune(insertBytes(file, pdta.second, unknown, { pdta.first })));

    // What it must still refuse: the same cases cut short, and a list that is not covered by its chunks
    // in a way that would make TinySoundFont read the next list from the wrong place.
    {
        auto cut = insertBytes(file, pdta.second, { 0, 0, 0, 0 }, { pdta.first });
        cut.resize(cut.size() - 60);
        CHECK(SoundFontBank::fromMemory(cut.data(), static_cast<int>(cut.size()), 48000.0) == nullptr);
    }
    {
        // Slack inside sdta pushes TinySoundFont's reading position into the slack, where it finds no
        // pdta; it fails to load such a file, and so does this.
        auto slack = insertBytes(file, sdta.second, { 0, 0, 0, 0 }, { sdta.first });
        CHECK(SoundFontBank::fromMemory(slack.data(), static_cast<int>(slack.size()), 48000.0) == nullptr);
    }
}

TEST(sf_audio_thread_does_not_allocate)
{
    TestSf2Options two;
    two.presets = { { "One", 0, 0 }, { "Two", 0, 1 } };
    const auto bank = makeBank(two);
    const auto otherBank = makeBank(two);
    auto engine = makeSf(bank.get());
    SoundFontParams changed;
    changed.instrument = 1;
    changed.velocityOn = false;
    changed.echoMix = 0.6f;
    changed.echoDelay = 15;
    changed.echoFeedback = 0.9f;
    std::vector<float> block(30000);

    const auto before = allocationCounts();
    for (int note = 40; note < 80; ++note) // 8 more than the 32 voices
    {
        engine->noteOn(note, 100);
        engine->render(block.data(), 64);
    }
    const int busy = engine->activeVoices();
    engine->setParams(changed);
    engine->setPitchBend(1.5);
    engine->render(block.data(), 30000);
    engine->setSustain(true);
    engine->noteOn(60, 100);
    engine->noteOff(60);
    engine->setSustain(false);
    engine->setBank(otherBank.get());
    engine->setParams({});
    engine->setParams(changed);
    engine->setParams({});
    engine->noteOn(70, 90);
    engine->render(block.data(), 512);
    engine->noteOff(70);
    engine->allNotesOff();
    engine->render(block.data(), 512);
    engine->setBank(nullptr);
    engine->noteOn(70, 90);
    engine->render(block.data(), 512);
    const auto after = allocationCounts();

    CHECK(busy == 32);
    CHECK(after.cpp == before.cpp);
    CHECK(after.tsf == before.tsf);
    CHECK(before.tsf > 0); // the counter does see what the bank allocated while loading
}

TEST(sf_silent_without_bank)
{
    SoundFontEngine engine;
    engine.prepare(48000.0);
    engine.noteOn(69, 127);
    CHECK(engine.activeVoices() == 0);
    std::vector<float> out(512, 1.0f);
    engine.render(out.data(), 512);
    CHECK(peak(out) == 0.0);

    // Nor before prepare, with a bank.
    const auto bank = makeBank();
    SoundFontEngine unprepared;
    unprepared.setBank(bank.get());
    unprepared.noteOn(69, 127);
    CHECK(unprepared.activeVoices() == 0);
    std::fill(out.begin(), out.end(), 1.0f);
    unprepared.render(out.data(), 512);
    CHECK(peak(out) == 0.0);

    // Taking the bank away silences what played.
    auto engine2 = makeSf(bank.get());
    engine2->noteOn(69, 127);
    render(*engine2, 480);
    engine2->setBank(nullptr);
    CHECK(engine2->activeVoices() == 0);
    CHECK(peak(render(*engine2, 480)) == 0.0);
    CHECK(engine2->latencySamples() == 0);
}

TEST(sf_finite_at_all_rates)
{
    const auto bank = makeBank();
    for (double sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        bank->setSampleRate(sampleRate);
        auto engine = makeSf(bank.get(), {}, sampleRate);
        for (int note : { 0, 40, 69, 127 })
            engine->noteOn(note, 127);
        engine->setPitchBend(2.0);
        const auto out = render(*engine, 24000);
        CHECK(allFinite(out));
        CHECK(peak(out) > 0.0);
        CHECK(engine->latencySamples() == 0);

        auto tuned = makeSf(bank.get(), {}, sampleRate);
        tuned->noteOn(69, 127);
        const auto held = render(*tuned, static_cast<int>(sampleRate));
        CHECK(std::abs(centsBetween(measureFrequency(held, sampleRate, 4800), noteFrequency(69))) < 10.0);
    }

    // The bank follows a change of rate made by prepare.
    auto engine = makeSf(bank.get(), {}, 44100.0);
    engine->prepare(96000.0);
    engine->noteOn(69, 127);
    const auto out = render(*engine, 96000);
    CHECK(std::abs(centsBetween(measureFrequency(out, 96000.0, 9600), noteFrequency(69))) < 10.0);
}

TEST(sf_large_and_empty_blocks)
{
    // A bank has one set of voices, so each engine gets its own.
    const auto wholeBank = makeBank();
    const auto splitBank = makeBank();
    for (double sampleRate : { 44100.0, 192000.0 })
    {
        auto whole = makeSf(wholeBank.get(), {}, sampleRate);
        auto split = makeSf(splitBank.get(), {}, sampleRate);
        whole->noteOn(69, 127);
        split->noteOn(69, 127);

        constexpr int total = 200000;
        const auto wholeOut = render(*whole, total);
        std::vector<float> splitOut(total, 0.0f);
        split->render(splitOut.data(), 0);
        split->render(splitOut.data(), -3);
        for (int done = 0, block = 1; done < total; block = block % 1000 + 37)
        {
            const int n = std::min(block, total - done);
            split->render(splitOut.data() + done, n);
            done += n;
        }

        double worst = 0.0;
        for (size_t i = 0; i < wholeOut.size(); ++i)
            worst = std::max(worst, std::abs(static_cast<double>(wholeOut[i]) - splitOut[i]));
        CHECK(worst < 1e-6);
        CHECK(std::abs(centsBetween(measureFrequency(wholeOut, sampleRate, 2000), noteFrequency(69))) < 10.0);
    }
}

TEST(sf_invalid_events_are_ignored)
{
    const auto bank = makeBank();
    auto engine = makeSf(bank.get());
    engine->noteOn(-1, 100);
    engine->noteOn(128, 100);
    engine->noteOff(60);
    engine->noteOff(-7);
    CHECK(engine->activeVoices() == 0);
    engine->noteOn(60, 100);
    engine->noteOn(60, 0); // velocity 0 is a note-off
    CHECK(engine->activeVoices() == 1);
    render(*engine, 24000);
    CHECK(engine->activeVoices() == 0);

    engine->noteOn(60, 1000); // too loud is full
    CHECK(allFinite(render(*engine, 2400)));
    engine->setPitchBend(1e6);
    CHECK(allFinite(render(*engine, 2400)));
    engine->setPitchBend(-1e6);
    CHECK(allFinite(render(*engine, 2400)));
    engine->setPitchBend(std::nan(""));
    CHECK(allFinite(render(*engine, 2400)));
    CHECK(engine->activeVoices() == 1);
}

TEST(sf_prepare_resets_voices)
{
    const auto bank = makeBank();
    auto engine = makeSf(bank.get(), {}, 44100.0);
    engine->setSustain(true);
    engine->setPitchBend(2.0);
    engine->noteOn(69, 127);
    render(*engine, 1000);
    engine->prepare(96000.0);
    CHECK(engine->activeVoices() == 0);
    CHECK(peak(render(*engine, 960)) == 0.0);
    engine->noteOn(69, 127);
    const auto out = render(*engine, 192000);
    CHECK(std::abs(centsBetween(measureFrequency(out, 96000.0, 9600), noteFrequency(69))) < 10.0);
    engine->noteOff(69);
    render(*engine, 48000);
    CHECK(engine->activeVoices() == 0);
}

TEST(sf_level_comparable_to_2a03)
{
    const auto bank = makeBank();
    auto engine = makeSf(bank.get());
    engine->noteOn(69, 127);
    const double sfLevelRms = rms(render(*engine, 48000), 4800);

    auto pulse = makeEngine(NesChannel::Pulse);
    pulse.noteOn(69, 127);
    const double pulseLevel = rms(render(pulse, 48000));
    std::printf("  SoundFont %.4f, 2A03 pulse %.4f, %+.2f dB\n", sfLevelRms, pulseLevel, decibels(sfLevelRms / pulseLevel));
    CHECK(std::abs(decibels(sfLevelRms / pulseLevel)) < 6.0);
}

// A file of one-zone presets that play the loud sample, each with the generators it is given.
std::vector<std::uint8_t> oneZonePresets(const std::vector<std::vector<testsf2::Zone>>& presetZones)
{
    using namespace testsf2;
    std::vector<PresetSpec> presets;
    std::vector<InstrumentSpec> instruments;
    for (size_t i = 0; i < presetZones.size(); ++i)
    {
        presets.push_back({ "P" + std::to_string(i), 0, static_cast<int>(i), { { { kInstrument, static_cast<int>(i) } } } });
        instruments.push_back({ "I" + std::to_string(i), presetZones[i] });
    }
    return buildSf2(presets, instruments);
}

// The level of A4 on the preset of that number, once the attack is over.
double presetLevel(SoundFontBank& bank, int preset)
{
    SoundFontEngine engine;
    engine.setBank(&bank);
    SoundFontParams params;
    params.instrument = preset;
    engine.setParams(params);
    engine.prepare(48000.0);
    engine.noteOn(69, 127);
    return rms(render(engine, 24000), 4800);
}

TEST(sf_attenuation_is_scaled_by_the_constant)
{
    // TinySoundFont plays the format's centibels a tenth as strong as written; the loader multiplies them back
    // by kAttenuationFix, so 200 cB (20 dB in the format) is 2 dB x the constant.
    using namespace testsf2;
    const Zone plain { { kSampleModes, 1 }, { kSampleId, 1 } };
    const Zone quieter { { kAttenuation, 200 }, { kSampleModes, 1 }, { kSampleId, 1 } };
    const auto file = oneZonePresets({ { plain }, { quieter } });
    const auto bank = SoundFontBank::fromMemory(file.data(), static_cast<int>(file.size()), 48000.0);
    CHECK(bank != nullptr);
    if (!bank)
        return;

    const double change = decibels(presetLevel(*bank, 1) / presetLevel(*bank, 0));
    const double expected = -2.0 * soundfont::kAttenuationFix;
    std::printf("  200 cB: %+.2f dB, expected %+.2f dB\n", change, expected);
    CHECK_NEAR(change, expected, 0.3);
    CHECK(soundfont::kAttenuationFix == 4.0f); // the value chosen for now; the listening test may change it
}

TEST(sf_stereo_pair_matches_a_mono_zone)
{
    // A stereo pair is two zones, one panned to each side, and the mono output ignores the pan, so the pair would
    // sum 6 dB above a mono zone. The loader takes the 6 dB off a hard-panned zone, and nothing off a centred one.
    using namespace testsf2;
    const Zone mono { { kSampleModes, 1 }, { kSampleId, 1 } };
    const Zone left { { kPan, -500 }, { kSampleModes, 1 }, { kSampleId, 1 } };
    const Zone right { { kPan, 500 }, { kSampleModes, 1 }, { kSampleId, 1 } };
    const Zone half { { kPan, 250 }, { kSampleModes, 1 }, { kSampleId, 1 } };
    const Zone centred { { kPan, 0 }, { kSampleModes, 1 }, { kSampleId, 1 } };
    const auto file = oneZonePresets({ { mono }, { left, right }, { centred }, { half }, { left } });
    const auto bank = SoundFontBank::fromMemory(file.data(), static_cast<int>(file.size()), 48000.0);
    CHECK(bank != nullptr);
    if (!bank)
        return;

    const double monoLevel = presetLevel(*bank, 0);
    const double pair = decibels(presetLevel(*bank, 1) / monoLevel);
    const double centredChange = decibels(presetLevel(*bank, 2) / monoLevel);
    const double halfChange = decibels(presetLevel(*bank, 3) / monoLevel);
    const double hard = decibels(presetLevel(*bank, 4) / monoLevel);
    std::printf("  pair %+.2f dB, centred %+.2f dB, half pan %+.2f dB, hard pan alone %+.2f dB\n", pair, centredChange, halfChange, hard);
    CHECK(std::abs(pair) < 1.0);
    CHECK(std::abs(centredChange) < 0.05);
    CHECK_NEAR(halfChange, -3.01, 0.2);
    CHECK_NEAR(hard, -6.02, 0.2);
}

TEST(sf_chord_of_stereo_pairs_stays_below_full_scale)
{
    using namespace testsf2;
    TestSf2Options options;
    options.highAmplitude = 1.0;
    const Zone left { { kPan, -500 }, { kSampleModes, 1 }, { kSampleId, 1 } };
    const Zone right { { kPan, 500 }, { kSampleModes, 1 }, { kSampleId, 1 } };
    const auto file = buildSf2({ { "Pair", 0, 0, { { { kInstrument, 0 } } } } }, { { "Pair", { left, right } } }, options);
    const auto bank = SoundFontBank::fromMemory(file.data(), static_cast<int>(file.size()), 48000.0);
    CHECK(bank != nullptr);
    if (!bank)
        return;

    auto engine = makeSf(bank.get());
    for (int note : { 69, 73, 76, 81 })
        engine->noteOn(note, 127);
    const double chordPeak = peak(render(*engine, 24000));
    std::printf("  four full-scale pairs in a chord: peak %.3f\n", chordPeak);
    CHECK(chordPeak < 1.0);
    CHECK(chordPeak > 0.1);
}

TEST(sf_32_voices_under_25_percent_cpu)
{
    TestSf2Options options;
    options.filterCents = 8000; // a closed filter, which costs more per voice than none
    const auto bank = makeBank(options);
    for (double sampleRate : { 44100.0, 192000.0 })
    {
        bank->setSampleRate(sampleRate);
        auto engine = makeSf(bank.get(), {}, sampleRate);
        for (int note = 40; note < 72; ++note)
            engine->noteOn(note, 127);

        std::vector<float> block(512);
        const auto start = std::chrono::steady_clock::now();
        constexpr int seconds = 10;
        const int blocks = static_cast<int>(seconds * sampleRate / 512);
        for (int i = 0; i < blocks; ++i)
            engine->render(block.data(), 512);
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        std::printf("  %.0f Hz: CPU %.1f %% of one core\n", sampleRate, 100.0 * elapsed.count() / seconds);
        CHECK(peak(block) > 0.01);
        CHECK(engine->activeVoices() == 32);
        CHECK(allFinite(block));
        CHECK(elapsed.count() < 0.25 * seconds);
    }
}


// ---- Echo at any rate, and in the SoundFont engine ---------------------------------------------

TEST(echo_keeps_its_durations_at_any_rate)
{
    for (double rate : { 32000.0, 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int steps : { 1, 4, 15 })
            for (bool delayFirst : { false, true })
            {
                SnesEcho echo;
                if (delayFirst)
                    echo.setDelay(steps);
                echo.prepare(rate);
                if (!delayFirst)
                    echo.setDelay(steps);

                const double wanted = steps * 0.016 * rate; // 16 ms a step
                std::vector<float> x(static_cast<size_t>(wanted) + 2000, 0.0f);
                x[0] = 1.0f;
                runEcho(echo, x, 1.0f, 0.0f);

                const size_t at = indexOfPeak(x, 1, x.size());
                if (!delayFirst && steps == 4)
                    std::printf("  %.0f Hz, 4 steps: peak at %zu, wanted %.1f (%.3f ms)\n", rate, at, wanted, 1000.0 * static_cast<double>(at) / rate);
                CHECK(std::abs(static_cast<double>(at) - wanted) <= 2.0);
                CHECK_NEAR(centroid(x, 1, x.size()), wanted, 2.0);
                CHECK_NEAR(x[at], 23.0 / 128.0, 1e-6); // the filter is the same at every rate
                double total = 0.0;
                for (size_t i = 1; i < x.size(); ++i)
                    total += x[i];
                CHECK_NEAR(total, 127.0 / 128.0, 1e-5);
            }

    // The feedback repeats one delay later at a high rate too, and 15 steps still fit the line.
    SnesEcho echo;
    echo.prepare(192000.0);
    echo.setDelay(15);
    std::vector<float> x(static_cast<size_t>(2.0 * 0.24 * 192000.0) + 3000, 0.0f);
    x[0] = 1.0f;
    runEcho(echo, x, 1.0f, 1.0f);
    CHECK_NEAR(centroid(x, 1, 69000), 46080.0, 2.0);
    CHECK_NEAR(centroid(x, 69000, x.size()), 2.0 * 46080.0, 3.0);
    CHECK(allFinite(x));

    // Changing the rate empties the line and keeps the setting; nonsense rates change nothing.
    SnesEcho again;
    again.prepare(48000.0);
    again.setDelay(2);
    std::vector<float> y(5000, 0.0f);
    y[0] = 1.0f;
    again.process(y.data(), 5000, 1.0f, 1.0f);
    again.prepare(96000.0);
    std::vector<float> z(12000, 0.0f);
    again.process(z.data(), 12000, 1.0f, 1.0f);
    CHECK(peak(z) == 0.0);
    z.assign(12000, 0.0f);
    z[0] = 1.0f;
    again.prepare(0.0);
    again.prepare(-5.0);
    again.process(z.data(), 12000, 1.0f, 0.0f);
    CHECK(std::abs(static_cast<double>(indexOfPeak(z, 1, z.size())) - 0.032 * 96000.0) <= 2.0);
}

TEST(echo_at_chip_rate_is_unchanged)
{
    // With no prepare, and with prepare(32000), the output is the same to the last bit, through
    // delay changes in the middle.
    SnesEcho plain, prepared;
    prepared.prepare(SnesEngine::kChipRate);
    TestNoise noise(23);
    for (int round = 0; round < 12; ++round)
    {
        const int steps = (round * 7) % (SnesEcho::kMaxSteps + 1);
        plain.setDelay(steps);
        prepared.setDelay(steps);
        std::vector<float> a(3000);
        for (auto& s : a)
            s = 0.5f * noise.next();
        auto b = a;
        runEcho(plain, a, 0.8f, 0.9f);
        runEcho(prepared, b, 0.8f, 0.9f);
        CHECK(a == b);
    }
    CHECK(SnesEcho::kBufferSize == SnesEcho::kMaxSteps * SnesEcho::kStepSamples);

    // The delay is the chip's whole steps of 512 samples.
    SnesEcho echo;
    echo.prepare(SnesEngine::kChipRate);
    echo.setDelay(15);
    std::vector<float> x(SnesEcho::kBufferSize + 1500, 0.0f);
    x[0] = 1.0f;
    runEcho(echo, x, 1.0f, 0.0f);
    CHECK(indexOfPeak(x, 1, x.size()) == static_cast<size_t>(SnesEcho::kMaxSteps * SnesEcho::kStepSamples));
}

TEST(sf_echo_reaches_the_output)
{
    // A short note, then silence: the echo is all that is heard afterwards.
    const auto burst = [&](float mix, int delay, double rate)
    {
        const auto bank = makeBank({}, rate);
        SoundFontParams params;
        params.echoMix = mix;
        params.echoDelay = delay;
        params.echoFeedback = 0.0f;
        auto engine = makeSf(bank.get(), params, rate);
        engine->noteOn(60, 127);
        auto out = render(*engine, static_cast<int>(0.01 * rate));
        engine->noteOff(60);
        const auto rest = render(*engine, static_cast<int>(0.4 * rate));
        out.insert(out.end(), rest.begin(), rest.end());
        return out;
    };

    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto dry = burst(0.0f, 4, rate);
        const auto wet = burst(1.0f, 4, rate);
        CHECK(allFinite(wet));
        // The mix is the only difference, and it starts 64 ms (4 steps) after the note did.
        size_t onset = wet.size();
        double echoPeak = 0.0;
        for (size_t i = 0; i < wet.size(); ++i)
        {
            const double d = std::abs(static_cast<double>(wet[i]) - dry[i]);
            if (d > 0.0 && onset == wet.size())
                onset = i;
            echoPeak = std::max(echoPeak, d);
        }
        std::printf("  %.0f Hz: echo starts at %.2f ms, peak %.4f\n", rate, 1000.0 * static_cast<double>(onset) / rate, echoPeak);
        CHECK(std::abs(static_cast<double>(onset) - 0.064 * rate) < 0.003 * rate);
        CHECK(echoPeak > 0.01);
    }

    // A longer delay arrives later: the echo is what the mix adds to the dry note and its release.
    const auto echoOnly = [&](int delay)
    {
        const auto dry = burst(0.0f, delay, 48000.0);
        auto wet = burst(1.0f, delay, 48000.0);
        for (size_t i = 0; i < wet.size(); ++i)
            wet[i] -= dry[i];
        return wet;
    };
    const auto firstAbove = [](const std::vector<float>& x)
    {
        for (size_t i = 0; i < x.size(); ++i)
            if (std::abs(x[i]) > 0.002)
                return i;
        return x.size();
    };
    const auto near = firstAbove(echoOnly(5));
    const auto far = firstAbove(echoOnly(12));
    CHECK(near < far);
    CHECK_NEAR(static_cast<double>(near), 0.080 * 48000.0, 0.003 * 48000.0);
    CHECK_NEAR(static_cast<double>(far), 0.192 * 48000.0, 0.003 * 48000.0);

    // Delay 0 is no echo either, whatever the mix; and settings out of range are held in range.
    CHECK(burst(1.0f, 0, 48000.0) == burst(0.0f, 4, 48000.0));
    CHECK(allFinite(burst(50.0f, 99, 48000.0)));
}

TEST(sf_echo_stays_stable_at_full_feedback)
{
    // The line dies away by 1/128 or more on each pass, so a short delay (many passes) fades fast,
    // and the longest delay (4 passes a second) slowly.
    for (double rate : { 44100.0, 192000.0 })
        for (int delay : { 4, 15 })
        {
            const auto bank = makeBank({}, rate);
            SoundFontParams params;
            params.echoMix = 1.0f;
            params.echoDelay = delay;
            params.echoFeedback = 1.0f;
            auto engine = makeSf(bank.get(), params, rate);
            const int notes[] = { 48, 55, 60, 64, 67, 72 };

            // A chord held for a few seconds: finite, and bounded.
            for (int note : notes)
                engine->noteOn(note, 127);
            double loudest = 0.0;
            bool finite = true;
            const int second = static_cast<int>(rate);
            for (int s = 0; s < 4; ++s)
            {
                const auto out = render(*engine, second);
                finite = finite && allFinite(out);
                loudest = std::max(loudest, peak(out));
            }

            // Then let go: the echo rings, and only dies away.
            for (int note : notes)
                engine->noteOff(note);
            render(*engine, second / 2);
            std::vector<double> peaks;
            for (int s = 0; s < 40; ++s)
            {
                const auto out = render(*engine, second);
                finite = finite && allFinite(out);
                peaks.push_back(peak(out));
            }
            std::printf("  %.0f Hz, delay %d: held peak %.3f, tail peaks %.4f (1 s) %.5f (10 s) %.6f (40 s)\n", rate, delay, loudest, peaks[1],
                        peaks[10], peaks[39]);
            CHECK(finite);
            CHECK(loudest < 8.0);
            CHECK(peaks[1] > 0.01);
            CHECK(peaks[10] < peaks[1]);
            CHECK(peaks[39] < peaks[10]);
            CHECK(peaks[39] < (delay == 4 ? 0.1 : 0.5) * peaks[1]);
        }
}

TEST(sf_all_off_clears_the_echo)
{
    const auto bank = makeBank();
    SoundFontParams params;
    params.echoMix = 1.0f;
    params.echoDelay = 2;
    params.echoFeedback = 1.0f;
    auto engine = makeSf(bank.get(), params);
    engine->noteOn(60, 127);
    engine->noteOn(64, 127);
    const auto playing = render(*engine, 24000);
    const double level = peak(playing);
    CHECK(level > 0.05);

    // The echo is running by now, so it would ring on for seconds.
    engine->allNotesOff();
    CHECK(engine->activeVoices() == 0);
    CHECK(peak(render(*engine, 96000)) == 0.0);

    // The next note starts clean, exactly as on a new engine.
    engine->noteOn(60, 127);
    const auto freshBank = makeBank();
    auto fresh = makeSf(freshBank.get(), params);
    fresh->noteOn(60, 127);
    const auto usedOut = render(*engine, 9600);
    const auto freshOut = render(*fresh, 9600);
    double worst = 0.0;
    for (size_t i = 0; i < usedOut.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(usedOut[i]) - freshOut[i]));
    CHECK(worst == 0.0);
    CHECK(rms(usedOut) > 0.01);

    // A change of instrument stops the notes the same way, and so does taking the bank away.
    TestSf2Options two;
    two.presets = { { "One", 0, 0 }, { "Two", 0, 1 } };
    const auto twoBank = makeBank(two);
    auto changing = makeSf(twoBank.get(), params);
    changing->noteOn(60, 127);
    render(*changing, 24000);
    auto next = params;
    next.instrument = 1;
    changing->setParams(next);
    CHECK(peak(render(*changing, 48000)) == 0.0);

    auto removing = makeSf(bank.get(), params);
    removing->noteOn(60, 127);
    render(*removing, 24000);
    removing->setBank(nullptr);
    removing->setBank(bank.get());
    CHECK(peak(render(*removing, 48000)) == 0.0);
}

TEST(bank_refuses_a_region_bomb)
{
    using namespace testsf2;
    const auto load = [](const std::vector<std::uint8_t>& file)
    {
        return SoundFontBank::fromMemory(file.data(), static_cast<int>(file.size()), 48000.0);
    };
    // A preset with `refs` references to an instrument of `zones` zones asks for refs x zones regions.
    const auto fontWith = [](int presets, int refs, int zones)
    {
        InstrumentSpec instrument { "Big", {} };
        for (int i = 0; i < zones; ++i)
            instrument.zones.push_back({ { kSampleId, 0 } });
        Zone reference;
        for (int i = 0; i < refs; ++i)
            reference.push_back({ kInstrument, 0 });
        std::vector<PresetSpec> list;
        for (int p = 0; p < presets; ++p)
            list.push_back({ "Bomb" + std::to_string(p), 0, p, { reference } });
        return buildSf2(list, { instrument });
    };
    const auto seconds = [&](const std::vector<std::uint8_t>& file, bool& loaded)
    {
        const auto start = std::chrono::steady_clock::now();
        loaded = load(file) != nullptr;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };

    bool loaded = true;

    // 4 million regions from a file of a few tens of kilobytes.
    const auto bomb = fontWith(1, 2000, 2000);
    const double bombTime = seconds(bomb, loaded);
    std::printf("  one preset, 4 M regions: %zu bytes, loaded=%d in %.3f s\n", bomb.size(), loaded, bombTime);
    CHECK(!loaded);
    CHECK(bombTime < 1.0);

    // Each preset is under the limit, and the file as a whole is over it.
    const auto spread = fontWith(3, 1000, 1000); // 3 x 1 M
    const double spreadTime = seconds(spread, loaded);
    std::printf("  three presets, 3 M regions: %zu bytes, loaded=%d in %.3f s\n", spread.size(), loaded, spreadTime);
    CHECK(!loaded);
    CHECK(spreadTime < 1.0);

    // Counts that overflow a 32-bit total, from a few references to a huge instrument.
    const auto huge = fontWith(2, 20000, 20000); // 400 M each
    const double hugeTime = seconds(huge, loaded);
    std::printf("  two presets, 800 M regions: %zu bytes, loaded=%d in %.3f s\n", huge.size(), loaded, hugeTime);
    CHECK(!loaded);
    CHECK(hugeTime < 1.0);

    // Many references to zones that hold no sample cost time without adding a region.
    InstrumentSpec empty { "Empty", {} };
    for (int i = 0; i < 20000; ++i)
        empty.zones.push_back({ { kInitialFilterFc, 13500 } });
    Zone many;
    for (int i = 0; i < 20000; ++i)
        many.push_back({ kInstrument, 0 });
    const auto idle = buildSf2({ { "Idle", 0, 0, { many } } }, { empty });
    const double idleTime = seconds(idle, loaded);
    std::printf("  400 M idle zone visits: %zu bytes, loaded=%d in %.3f s\n", idle.size(), loaded, idleTime);
    CHECK(!loaded);
    CHECK(idleTime < 1.0);

    // A font with a normal count still loads, and so does a large one under the limit.
    const auto normal = fontWith(1, 20, 20);
    CHECK(load(normal) != nullptr);
    CHECK(load(buildTestSf2()) != nullptr);
    const auto bank = load(fontWith(2, 100, 100));
    CHECK(bank != nullptr);
    if (bank)
        playEverything(*bank);
}


// A file whose every field is different, so that a field read from the wrong place shows.
std::array<std::uint8_t, kTfiSize> tfiFile()
{
    std::array<std::uint8_t, kTfiSize> file {};
    file[0] = 4;
    file[1] = 5;
    for (size_t block = 0; block < 4; ++block)
    {
        const auto i = static_cast<std::uint8_t>(block);
        const size_t at = 2 + 10 * block;
        file[at + 0] = static_cast<std::uint8_t>(i + 1);   // mul
        file[at + 1] = 3;                                  // dt, the centre
        file[at + 2] = static_cast<std::uint8_t>(40 + i);  // tl
        file[at + 3] = i;                                  // rs
        file[at + 4] = static_cast<std::uint8_t>(20 + i);  // ar
        file[at + 5] = static_cast<std::uint8_t>(10 + i);  // dr
        file[at + 6] = static_cast<std::uint8_t>(5 + i);   // sr
        file[at + 7] = static_cast<std::uint8_t>(1 + i);   // rr
        file[at + 8] = static_cast<std::uint8_t>(11 + i);  // sl
    }
    return file;
}

TEST(tfi_reads_every_field_in_register_order)
{
    const auto file = tfiFile();
    const auto patch = parseTfi(file.data(), file.size());
    CHECK(patch.has_value());
    if (!patch)
        return;

    CHECK(patch->algorithm == 4);
    CHECK(patch->feedback == 5);

    // The file's blocks follow the registers: operators 1, 3, 2, 4.
    const int blockOfOperator[] = { 0, 2, 1, 3 };
    for (size_t op = 0; op < 4; ++op)
    {
        const int block = blockOfOperator[op];
        const auto& o = patch->ops[op];
        CHECK(o.mul == block + 1);
        CHECK(o.dt == 0);
        CHECK(o.tl == 40 + block);
        CHECK(o.rs == block);
        CHECK(o.ar == 20 + block);
        CHECK(o.dr == 10 + block);
        CHECK(o.sr == 5 + block);
        CHECK(o.rr == 1 + block);
        CHECK(o.sl == 11 + block);
        CHECK(o.ssg == 0);
    }
}

TEST(tfi_converts_detune_to_the_register_coding)
{
    // 0 to 6 in the file, with 3 for no detune. The register has 1 to 3 sharp and 5 to 7 flat.
    const int expected[] = { 7, 6, 5, 0, 1, 2, 3 };
    for (int value = 0; value <= 6; ++value)
    {
        auto file = tfiFile();
        file[3] = static_cast<std::uint8_t>(value);
        const auto patch = parseTfi(file.data(), file.size());
        CHECK(patch.has_value());
        if (patch)
            CHECK(patch->ops[0].dt == expected[value]);
    }
}

TEST(tfi_keeps_ssg_eg_only_when_enabled)
{
    for (int value : { 0, 7, 8, 15 })
    {
        auto file = tfiFile();
        file[11] = static_cast<std::uint8_t>(value);
        const auto patch = parseTfi(file.data(), file.size());
        CHECK(patch.has_value());
        if (patch)
            CHECK(patch->ops[0].ssg == (value < 8 ? 0 : value));
    }
}

TEST(tfi_rejects_what_is_not_a_timbre)
{
    const auto good = tfiFile();
    CHECK(!parseTfi(nullptr, kTfiSize));
    CHECK(!parseTfi(good.data(), 0));
    CHECK(!parseTfi(good.data(), kTfiSize - 1));
    std::vector<std::uint8_t> longer(good.begin(), good.end());
    longer.push_back(0);
    CHECK(!parseTfi(longer.data(), longer.size()));

    const std::pair<size_t, std::uint8_t> tooHigh[] = { { 0, 8 },  { 1, 8 },  { 2, 16 }, { 3, 7 },  { 4, 128 }, { 5, 4 },
                                                        { 6, 32 }, { 7, 32 }, { 8, 32 }, { 9, 16 }, { 10, 16 }, { 11, 16 },
                                                        { 41, 16 } };
    for (const auto& [offset, value] : tooHigh)
    {
        auto file = good;
        file[offset] = value;
        CHECK(!parseTfi(file.data(), file.size()));
    }
}

GenesisParams userPatchParams(const FmPatch& patch)
{
    GenesisParams params;
    params.patch = GenesisEngine::kUserPatch;
    params.algorithm = patch.algorithm;
    params.feedback = patch.feedback;
    return params;
}

TEST(genesis_user_patch_sounds_like_the_same_builtin)
{
    GenesisUserSound sound;
    sound.patch = kFmPatches[kPatchEPiano];

    auto builtin = makeGenesis(paramsForPatch(kPatchEPiano));
    auto user = makeGenesis(userPatchParams(*sound.patch));
    user->setUserSound(&sound);
    builtin->noteOn(60, 100);
    user->noteOn(60, 100);
    const auto expected = render(*builtin, 9600);
    const auto played = render(*user, 9600);
    CHECK(rms(played) > 0.01);

    double difference = 0.0;
    for (size_t i = 0; i < played.size(); ++i)
        difference = std::max(difference, static_cast<double>(std::abs(played[i] - expected[i])));
    CHECK(difference < 1e-6);
}

TEST(genesis_ssg_eg_reaches_the_chip)
{
    // A decay to silence, which SSG-EG mode 8 repeats for as long as the key is down.
    FmPatch patch { "", 7, 0, { { 1, 0, 127, 0, 31, 0, 0, 0, 10 }, { 1, 0, 127, 0, 31, 0, 0, 0, 10 },
                                { 1, 0, 127, 0, 31, 0, 0, 0, 10 }, { 1, 0, 0, 0, 31, 24, 24, 15, 10 } } };
    const auto lateLevel = [&](int ssg)
    {
        GenesisUserSound sound;
        sound.patch = patch;
        sound.patch->ops[3].ssg = ssg;
        auto engine = makeGenesis(userPatchParams(*sound.patch));
        engine->setUserSound(&sound);
        engine->noteOn(60, 127);
        render(*engine, 48000);
        return rms(render(*engine, 9600));
    };
    CHECK(lateLevel(0) < 1e-3);
    CHECK(lateLevel(8) > 0.01);
}

TEST(genesis_imported_patches_are_silent_until_they_exist)
{
    for (int patch : { GenesisEngine::kUserPatch, GenesisEngine::kPcmPatch })
    {
        GenesisParams params;
        params.patch = patch;
        auto engine = makeGenesis(params);
        engine->noteOn(60, 127);
        CHECK(engine->activeVoices() == 0);
        CHECK(peak(render(*engine, 4800)) == 0.0);

        // One kind of import doesn't stand in for the other.
        GenesisUserSound sound;
        if (patch == GenesisEngine::kUserPatch)
            sound.pcm.assign(100, 200);
        else
            sound.patch = kFmPatches[0];
        engine->setUserSound(&sound);
        engine->noteOn(60, 127);
        CHECK(engine->activeVoices() == 0);
        engine->setUserSound(nullptr);
    }
}

TEST(genesis_replacing_the_user_sound_stops_only_its_notes)
{
    GenesisUserSound first, second;
    first.patch = second.patch = kFmPatches[kPatchOrgan];

    auto engine = makeGenesis(userPatchParams(*first.patch));
    engine->setUserSound(&first);
    engine->noteOn(60, 127);
    render(*engine, 480);
    CHECK(engine->activeVoices() == 1);
    engine->setUserSound(&second);
    CHECK(engine->activeVoices() == 0);

    engine->setParams(paramsForPatch(kPatchOrgan));
    engine->noteOn(60, 127);
    render(*engine, 480);
    engine->setUserSound(&first);
    CHECK(engine->activeVoices() == 1);
}

std::vector<std::uint8_t> sinePcm(double frequency, double seconds)
{
    std::vector<std::uint8_t> pcm(static_cast<size_t>(seconds * GenesisEngine::kPcmRate));
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<std::uint8_t>(std::lround(128.0 + 127.0 * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / GenesisEngine::kPcmRate)));
    return pcm;
}

GenesisParams pcmParams(int root = 69, bool loop = false)
{
    GenesisParams params;
    params.patch = GenesisEngine::kPcmPatch;
    params.pcmRootNote = root;
    params.pcmLoop = loop;
    return params;
}

std::unique_ptr<GenesisEngine> makePcm(const GenesisUserSound& sound, const GenesisParams& params)
{
    auto engine = makeGenesis(params);
    engine->setUserSound(&sound);
    return engine;
}

TEST(genesis_pcm_pitch_follows_the_root_note)
{
    GenesisUserSound sound;
    sound.pcm = sinePcm(440.0, 3.0);

    const auto playedFrequency = [&](int root, int note)
    {
        auto engine = makePcm(sound, pcmParams(root));
        engine->noteOn(note, 127);
        return measureFrequency(render(*engine, 48000), 48000.0, 4800);
    };
    CHECK(std::abs(centsBetween(playedFrequency(69, 69), 440.0)) < 5.0);
    CHECK(std::abs(centsBetween(playedFrequency(69, 81), 880.0)) < 5.0);
    CHECK(std::abs(centsBetween(playedFrequency(57, 69), 880.0)) < 5.0);
    CHECK(std::abs(centsBetween(playedFrequency(69, 57), 220.0)) < 5.0);
}

TEST(genesis_pcm_pitch_follows_the_bend_and_the_glide)
{
    GenesisUserSound sound;
    sound.pcm = sinePcm(440.0, 3.0);

    auto bent = makePcm(sound, pcmParams());
    bent->noteOn(69, 127);
    bent->setPitchBend(2.0);
    render(*bent, 4800);
    CHECK(std::abs(centsBetween(measureFrequency(render(*bent, 24000), 48000.0), noteFrequency(71))) < 5.0);

    auto params = pcmParams(69, true);
    params.glideMs = 200.0f;
    auto glide = makePcm(sound, params);
    glide->noteOn(57, 127);
    render(*glide, 4800);
    glide->noteOn(81, 127);
    glide->noteOff(57);
    render(*glide, 19200);
    CHECK(std::abs(centsBetween(measureFrequency(render(*glide, 24000), 48000.0), 880.0)) < 5.0);
}

TEST(genesis_pcm_level_is_that_of_a_full_fm_channel)
{
    GenesisUserSound sound;
    sound.pcm = sinePcm(440.0, 1.0);
    auto engine = makePcm(sound, pcmParams());
    engine->noteOn(69, 127);
    const double level = peak(render(*engine, 24000));
    std::printf("  peak %.3f\n", level);
    CHECK(level > 0.13 && level < 0.2);
}

TEST(genesis_pcm_without_loop_plays_to_its_end_after_the_key)
{
    GenesisUserSound sound;
    sound.pcm = sinePcm(440.0, 0.2);
    auto engine = makePcm(sound, pcmParams());
    engine->noteOn(69, 127);
    render(*engine, 2400);
    engine->noteOff(69);
    CHECK(engine->activeVoices() == 1);
    CHECK(rms(render(*engine, 2400)) > 0.05);

    render(*engine, 9600);
    CHECK(engine->activeVoices() == 0);
    render(*engine, 9600); // the DC blocker settles
    CHECK(rms(render(*engine, 2400)) < 1e-3);
}

TEST(genesis_pcm_loop_holds_until_the_key_then_fades)
{
    GenesisUserSound sound;
    sound.pcm = sinePcm(440.0, 0.05);

    const auto activeAfterRelease = [&](float release, int samples)
    {
        auto params = pcmParams(69, true);
        params.release = release;
        auto engine = makePcm(sound, params);
        engine->noteOn(69, 127);
        render(*engine, 48000);
        CHECK(engine->activeVoices() == 1);
        CHECK(rms(render(*engine, 2400)) > 0.05);
        engine->noteOff(69);
        render(*engine, samples);
        return engine->activeVoices() == 1;
    };
    CHECK(activeAfterRelease(-1.0f, 480));
    CHECK(!activeAfterRelease(-1.0f, 1920));
    CHECK(activeAfterRelease(0.0f, 7200));
    CHECK(!activeAfterRelease(0.0f, 12000));
    CHECK(activeAfterRelease(1.0f, 88000));
    CHECK(!activeAfterRelease(1.0f, 100000));
}

TEST(genesis_pcm_fade_has_no_click)
{
    GenesisUserSound sound;
    sound.pcm = sinePcm(440.0, 0.05);
    auto engine = makePcm(sound, pcmParams(69, true));
    engine->noteOn(69, 127);
    render(*engine, 24000);
    engine->noteOff(69);
    const double early = rms(render(*engine, 960));
    render(*engine, 3840);
    const double middle = rms(render(*engine, 960));
    CHECK(middle < early);
    CHECK(middle > 0.1 * early);
}

TEST(genesis_pcm_pedal_holds_a_looped_note)
{
    GenesisUserSound sound;
    sound.pcm = sinePcm(440.0, 0.05);
    auto engine = makePcm(sound, pcmParams(69, true));
    engine->setSustain(true);
    engine->noteOn(69, 127);
    engine->noteOff(69);
    render(*engine, 48000);
    CHECK(rms(render(*engine, 2400)) > 0.05);
    engine->setSustain(false);
    render(*engine, 24000);
    CHECK(engine->activeVoices() == 0);
}

TEST(genesis_pcm_chord_mixes_its_voices)
{
    GenesisUserSound sound;
    sound.pcm = sinePcm(440.0, 3.0);
    auto engine = makePcm(sound, pcmParams());
    engine->noteOn(69, 90);
    engine->noteOn(76, 90);
    CHECK(engine->activeVoices() == 2);
    const auto out = render(*engine, 48000);
    const double low = magnitudeAt(out, 440.0, 48000.0);
    CHECK(low > 0.01);
    CHECK(magnitudeAt(out, noteFrequency(76), 48000.0) > 0.5 * low);
}

TEST(genesis_pcm_mix_stays_in_the_dac_range)
{
    GenesisUserSound sound;
    sound.pcm.assign(26634, 255);
    for (size_t i = 0; i < sound.pcm.size(); i += 2000)
        std::fill_n(sound.pcm.begin() + static_cast<std::ptrdiff_t>(i), 1000, static_cast<std::uint8_t>(1));
    auto engine = makePcm(sound, pcmParams());
    for (int note = 69; note < 77; ++note)
        engine->noteOn(note, 127);
    const auto out = render(*engine, 24000);
    CHECK(std::all_of(out.begin(), out.end(), [](float s) { return std::isfinite(s); }));
    CHECK(peak(out) < 0.4); // one full-scale channel with the DC blocker's overshoot, not eight
}

TEST(genesis_pcm_velocity_follows_the_fm_range)
{
    GenesisUserSound sound;
    sound.pcm = sinePcm(440.0, 1.0);
    const auto level = [&](int velocity, bool on)
    {
        auto params = pcmParams();
        params.velocityOn = on;
        auto engine = makePcm(sound, params);
        engine->noteOn(69, velocity);
        return rms(render(*engine, 24000));
    };
    CHECK_NEAR(decibels(level(1, true) / level(127, true)), -18.0, 1.5);
    CHECK_NEAR(decibels(level(1, false) / level(127, false)), 0.0, 0.1);
}

TEST(genesis_pcm_is_quantized_to_8_bits_after_the_mix)
{
    // One step up and down: heard at full level, and rounded to nothing once the velocity brings it down.
    GenesisUserSound sound;
    sound.pcm.resize(26634);
    for (size_t i = 0; i < sound.pcm.size(); ++i)
        sound.pcm[i] = (i / 15) % 2 == 0 ? 129 : 127;

    const auto level = [&](int velocity)
    {
        auto engine = makePcm(sound, pcmParams());
        engine->noteOn(69, velocity);
        return rms(render(*engine, 24000));
    };
    CHECK(level(127) > 5e-4);
    CHECK(level(1) == 0.0);
}

TEST(genesis_pcm_stops_with_all_notes_off)
{
    GenesisUserSound sound;
    sound.pcm = sinePcm(440.0, 3.0);
    auto engine = makePcm(sound, pcmParams());
    engine->noteOn(69, 127);
    render(*engine, 2400);
    engine->allNotesOff();
    CHECK(engine->activeVoices() == 0);
    render(*engine, 9600);
    CHECK(rms(render(*engine, 2400)) < 1e-3);
}

TEST(genesis_switching_between_fm_and_pcm_leaves_nothing_behind)
{
    GenesisUserSound sound;
    sound.pcm = sinePcm(440.0, 3.0);
    auto engine = makePcm(sound, paramsForPatch(kPatchOrgan));

    engine->noteOn(60, 127);
    render(*engine, 2400);
    engine->setParams(pcmParams());
    CHECK(engine->activeVoices() == 0);
    render(*engine, 9600);
    CHECK(rms(render(*engine, 2400)) < 1e-3);

    engine->noteOn(69, 127);
    CHECK(rms(render(*engine, 4800)) > 0.05);
    engine->setParams(paramsForPatch(kPatchPureTone));
    CHECK(engine->activeVoices() == 0);
    render(*engine, 9600);
    CHECK(rms(render(*engine, 2400)) < 1e-3);

    engine->noteOn(69, 127);
    const auto fm = render(*engine, 96000);
    CHECK(std::abs(centsBetween(measureFrequency(fm, 48000.0, 4800), 440.0)) < 10.0);
}

TEST(genesis_pcm_and_user_patch_do_not_allocate)
{
    GenesisUserSound sound, other;
    sound.pcm = sinePcm(440.0, 0.05);
    sound.patch = kFmPatches[kPatchEPiano];
    other = sound;

    auto looped = pcmParams(69, true);
    looped.vibratoDepthCents = 30.0f;
    auto engine = makePcm(sound, looped);
    const auto user = userPatchParams(*sound.patch);
    std::vector<float> block(30000);

    const auto before = allocationCount.load();
    for (int note = 60; note < 72; ++note)
    {
        engine->noteOn(note, 100);
        engine->render(block.data(), 64);
    }
    engine->render(block.data(), 30000);
    engine->noteOff(61);
    engine->render(block.data(), 512);
    engine->setUserSound(&other);
    engine->noteOn(70, 90);
    engine->render(block.data(), 512);
    engine->setParams(user);
    engine->noteOn(70, 90);
    engine->render(block.data(), 512);
    engine->allNotesOff();
    engine->render(block.data(), 512);
    const auto after = allocationCount.load();

    CHECK(after == before);
}

GenesisParams kitParams()
{
    GenesisParams params;
    params.patch = GenesisEngine::kPcmKitPatch;
    return params;
}

// Kick on note 36 at 200 Hz, snare on note 38 at 800 Hz, both 3 s unless told otherwise.
GenesisUserSound twoPieceKit(double seconds = 3.0)
{
    GenesisUserSound sound;
    sound.kit.push_back({ 36, sinePcm(200.0, seconds), "kick" });
    sound.kit.push_back({ 38, sinePcm(800.0, seconds), "snare" });
    return sound;
}

TEST(genesis_kit_plays_each_key_at_its_recorded_pitch)
{
    const auto sound = twoPieceKit();
    const auto playedFrequency = [&](int note)
    {
        auto engine = makePcm(sound, kitParams());
        engine->noteOn(note, 127);
        return measureFrequency(render(*engine, 48000), 48000.0, 4800);
    };
    CHECK(std::abs(centsBetween(playedFrequency(36), 200.0)) < 5.0);
    CHECK(std::abs(centsBetween(playedFrequency(38), 800.0)) < 5.0);
}

TEST(genesis_kit_ignores_keys_without_a_sample)
{
    const auto sound = twoPieceKit();
    auto engine = makePcm(sound, kitParams());
    for (int note : { 0, 35, 37, 39, 127 })
    {
        engine->noteOn(note, 127);
        CHECK(engine->activeVoices() == 0);
    }
    CHECK(peak(render(*engine, 4800)) == 0.0);
}

TEST(genesis_kit_is_silent_until_it_exists)
{
    GenesisUserSound other;
    other.pcm = sinePcm(440.0, 1.0);
    other.patch = kFmPatches[0];
    auto engine = makePcm(other, kitParams());
    engine->noteOn(36, 127);
    CHECK(engine->activeVoices() == 0);

    // And the other PCM timbre doesn't play the kit.
    const auto kit = twoPieceKit();
    auto single = makePcm(kit, pcmParams(36));
    single->noteOn(36, 127);
    CHECK(single->activeVoices() == 0);
}

TEST(genesis_kit_plays_to_the_end_whatever_the_key_and_the_loop)
{
    const auto sound = twoPieceKit(0.2);
    auto params = kitParams();
    params.pcmLoop = true; // it belongs to the other timbre
    auto engine = makePcm(sound, params);
    engine->noteOn(36, 127);
    render(*engine, 2400);
    engine->noteOff(36);
    CHECK(engine->activeVoices() == 1);
    CHECK(rms(render(*engine, 2400)) > 0.05);
    render(*engine, 9600);
    CHECK(engine->activeVoices() == 0);
    render(*engine, 9600);
    CHECK(rms(render(*engine, 2400)) < 1e-3);
}

TEST(genesis_kit_keeps_its_pitch_under_bend_vibrato_and_glide)
{
    const auto sound = twoPieceKit();
    auto params = kitParams();
    params.vibratoDepthCents = 100.0f;
    params.glideMs = 500.0f;
    auto engine = makePcm(sound, params);
    engine->setPitchBend(2.0);
    engine->noteOn(36, 127);
    engine->noteOn(38, 127);
    engine->noteOff(36);
    render(*engine, 4800);

    const auto out = render(*engine, 48000);
    CHECK(magnitudeAt(out, 800.0, 48000.0) > 2.0 * magnitudeAt(out, 800.0 * std::pow(2.0, 2.0 / 12.0), 48000.0));
}

TEST(genesis_kit_hits_mix_and_retrigger)
{
    const auto sound = twoPieceKit();
    auto engine = makePcm(sound, kitParams());
    engine->noteOn(36, 90);
    engine->noteOn(38, 90);
    CHECK(engine->activeVoices() == 2);
    const auto out = render(*engine, 48000);
    CHECK(magnitudeAt(out, 200.0, 48000.0) > 0.01);
    CHECK(magnitudeAt(out, 800.0, 48000.0) > 0.01);

    // A second hit on a key takes the voice back to the start of the sample.
    const auto shortKit = twoPieceKit(0.2);
    auto again = makePcm(shortKit, kitParams());
    again->noteOn(36, 127);
    render(*again, 7200);
    again->noteOn(36, 127);
    CHECK(again->activeVoices() == 1);
    render(*again, 4800);
    CHECK(again->activeVoices() == 1);
}

TEST(genesis_kit_follows_the_velocity)
{
    const auto sound = twoPieceKit();
    const auto level = [&](int velocity)
    {
        auto engine = makePcm(sound, kitParams());
        engine->noteOn(36, velocity);
        return rms(render(*engine, 24000));
    };
    CHECK_NEAR(decibels(level(1) / level(127)), -18.0, 1.5);
}

TEST(genesis_replacing_the_kit_stops_its_notes)
{
    const auto first = twoPieceKit();
    const auto second = twoPieceKit();
    auto engine = makePcm(first, kitParams());
    engine->noteOn(36, 127);
    render(*engine, 480);
    engine->setUserSound(&second);
    CHECK(engine->activeVoices() == 0);
}

TEST(genesis_kit_does_not_allocate)
{
    const auto sound = twoPieceKit(0.05);
    const auto other = twoPieceKit(0.05);
    auto engine = makePcm(sound, kitParams());
    std::vector<float> block(30000);

    const auto before = allocationCount.load();
    for (int i = 0; i < 12; ++i)
    {
        engine->noteOn(36 + i % 4, 100);
        engine->render(block.data(), 512);
    }
    engine->render(block.data(), 30000);
    engine->setUserSound(&other);
    engine->noteOn(38, 90);
    engine->render(block.data(), 512);
    engine->allNotesOff();
    const auto after = allocationCount.load();

    CHECK(after == before);
}

std::unique_ptr<PsgEngine> makePsg(const PsgParams& params = {}, double sampleRate = 48000.0)
{
    auto engine = std::make_unique<PsgEngine>();
    engine->setParams(params);
    engine->prepare(sampleRate);
    return engine;
}

// The note the chip's divider really gives, which is what a Genesis plays.
double psgToneFrequency(int note)
{
    const auto divider = std::clamp(std::lround(PsgEngine::kClock / (32.0 * noteFrequency(note))), 1L, 1023L);
    return PsgEngine::kClock / (32.0 * static_cast<double>(divider));
}

// How much a signal looks like itself `lag` samples later. On a buzz it reads a pitch better than zero crossings do.
double normalizedAutocorrelation(const std::vector<float>& x, size_t lag, size_t from)
{
    double sum = 0.0, energy = 0.0;
    for (size_t i = from; i + lag < x.size(); ++i)
    {
        sum += static_cast<double>(x[i]) * x[i + lag];
        energy += static_cast<double>(x[i]) * x[i];
    }
    return energy > 0.0 ? sum / energy : 0.0;
}

TEST(psg_noise_register_has_the_segas_periods)
{
    const auto period = [](bool white)
    {
        std::uint16_t state = 0x8000;
        for (int steps = 1; steps <= 70000; ++steps)
        {
            state = PsgEngine::nextNoise(state, white);
            if (state == 0x8000)
                return steps;
        }
        return -1;
    };
    CHECK(period(false) == 16);
    CHECK(period(true) == 57337);
}

TEST(psg_tone_pitch_is_the_dividers)
{
    for (int note : { 45, 60, 69, 84, 96 })
    {
        auto engine = makePsg();
        engine->noteOn(note, 127);
        const double frequency = measureFrequency(render(*engine, 96000), 48000.0, 4800);
        std::printf("  note %d: %.2f Hz, chip %.2f Hz\n", note, frequency, psgToneFrequency(note));
        CHECK(std::abs(centsBetween(frequency, psgToneFrequency(note))) < 3.0);
    }
    // In tune where the chip can be, the middle of the keyboard above all.
    CHECK(std::abs(centsBetween(psgToneFrequency(69), 440.0)) < 3.0);
}

TEST(psg_tone_stays_at_its_limit_below_the_chips_range)
{
    const double limit = PsgEngine::kClock / (32.0 * PsgEngine::kMaxDivider);
    for (int note : { 36, 40, 44 })
    {
        auto engine = makePsg();
        engine->noteOn(note, 127);
        CHECK(std::abs(centsBetween(measureFrequency(render(*engine, 96000), 48000.0, 4800), limit)) < 3.0);
    }
}

TEST(psg_clock_gives_the_same_pitch_at_every_host_rate)
{
    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        auto engine = makePsg({}, rate);
        engine->noteOn(69, 127);
        const auto out = render(*engine, static_cast<int>(rate));
        CHECK(std::abs(centsBetween(measureFrequency(out, rate, static_cast<size_t>(rate / 10.0)), psgToneFrequency(69))) < 3.0);
    }
}

TEST(psg_tone_is_a_square)
{
    auto engine = makePsg();
    engine->noteOn(57, 127);
    const auto out = render(*engine, 96000);
    const double fundamental = psgToneFrequency(57);
    CHECK(magnitudeAt(out, 3.0 * fundamental, 48000.0) > 0.25 * magnitudeAt(out, fundamental, 48000.0));
    CHECK(magnitudeAt(out, 2.0 * fundamental, 48000.0) < 0.02 * magnitudeAt(out, fundamental, 48000.0));
}

TEST(psg_volume_goes_in_2_db_steps)
{
    const auto level = [](int sustain)
    {
        PsgParams params;
        params.sustain = sustain;
        auto engine = makePsg(params);
        engine->noteOn(69, 127);
        render(*engine, 4800);
        return rms(render(*engine, 24000));
    };
    const double full = level(15);
    CHECK_NEAR(decibels(level(14) / full), -2.0, 0.2);
    CHECK_NEAR(decibels(level(8) / full), -14.0, 0.3);
    CHECK_NEAR(decibels(level(1) / full), -28.0, 0.5);
}

TEST(psg_level_is_close_to_the_other_chips)
{
    auto engine = makePsg();
    engine->noteOn(69, 127);
    render(*engine, 2400);
    const double level = rms(render(*engine, 24000));
    std::printf("  rms %.3f\n", level);
    CHECK(level > 0.09 && level < 0.16);
}

TEST(psg_envelope_moves_only_at_the_60_hz_step)
{
    PsgParams params;
    params.attackMs = 500.0f;
    auto engine = makePsg(params);
    engine->noteOn(69, 127);
    const auto out = render(*engine, 24000);

    // Over half a second, level by level: one short window per step.
    double previous = 0.0;
    int rises = 0;
    for (size_t from = 0; from + 800 < out.size(); from += 800)
    {
        const std::vector<float> window(out.begin() + static_cast<std::ptrdiff_t>(from), out.begin() + static_cast<std::ptrdiff_t>(from + 800));
        const double level = rms(window);
        rises += level > previous * 1.05 ? 1 : 0;
        previous = level;
    }
    CHECK(rises >= 8);
    CHECK(rises <= 16); // never more than the chip has volumes
}

TEST(psg_attack_and_decay_take_their_time)
{
    PsgParams params;
    params.attackMs = 200.0f;
    params.decayMs = 200.0f;
    params.sustain = 8;
    auto engine = makePsg(params);
    engine->noteOn(69, 127);
    const double early = rms(render(*engine, 1200));
    render(*engine, 6600);
    const double peakLevel = rms(render(*engine, 2400));
    render(*engine, 19200);
    const double held = rms(render(*engine, 9600));
    CHECK(early < 0.5 * peakLevel);
    CHECK_NEAR(decibels(held / rms(render(*engine, 9600))), 0.0, 0.1);
    CHECK(held < peakLevel);
}

TEST(psg_note_off_releases_and_ends)
{
    PsgParams params;
    params.releaseMs = 200.0f;
    auto engine = makePsg(params);
    engine->noteOn(69, 127);
    render(*engine, 2400);
    engine->noteOff(69);
    CHECK(engine->activeVoices() == 1);
    render(*engine, 4800);
    CHECK(engine->activeVoices() == 1);
    render(*engine, 7200);
    CHECK(engine->activeVoices() == 0);
    render(*engine, 9600);
    CHECK(rms(render(*engine, 2400)) < 1e-3);
}

TEST(psg_note_with_no_release_ends_at_once)
{
    PsgParams params;
    params.releaseMs = 0.0f;
    auto engine = makePsg(params);
    engine->noteOn(69, 127);
    render(*engine, 2400);
    engine->noteOff(69);
    CHECK(engine->activeVoices() == 0);
}

TEST(psg_sustain_zero_frees_the_voice_after_the_decay)
{
    PsgParams params;
    params.decayMs = 100.0f;
    params.sustain = 0;
    auto engine = makePsg(params);
    engine->noteOn(69, 127);
    CHECK(engine->activeVoices() == 1);
    render(*engine, 9600);
    CHECK(engine->activeVoices() == 0);
}

TEST(psg_velocity_lowers_the_volume_by_steps)
{
    const auto level = [](int velocity, bool on)
    {
        PsgParams params;
        params.velocityOn = on;
        auto engine = makePsg(params);
        engine->noteOn(69, velocity);
        render(*engine, 2400);
        return rms(render(*engine, 24000));
    };
    CHECK_NEAR(decibels(level(1, true) / level(127, true)), -16.0, 0.5);
    CHECK_NEAR(decibels(level(1, false) / level(127, false)), 0.0, 0.1);
}

TEST(psg_white_noise_does_not_repeat_and_follows_its_rate)
{
    const auto noise = [](int rate, int note)
    {
        PsgParams params;
        params.noise = true;
        params.noiseRate = rate;
        auto engine = makePsg(params);
        engine->noteOn(note, 127);
        render(*engine, 2400);
        return render(*engine, 48000);
    };
    const auto high = noise(0, 60);
    CHECK(rms(high) > 0.05);
    for (size_t lag : { 137u, 500u, 1000u })
        CHECK(std::abs(normalizedAutocorrelation(high, lag, 0)) < 0.2);

    // The lower the rate, the more the signal looks like itself one sample later.
    const double highSmooth = normalizedAutocorrelation(high, 1, 0);
    const double midSmooth = normalizedAutocorrelation(noise(1, 60), 1, 0);
    const double lowSmooth = normalizedAutocorrelation(noise(2, 60), 1, 0);
    CHECK(highSmooth < midSmooth);
    CHECK(midSmooth < lowSmooth);

    // The fixed rates don't follow the key, and the last one does.
    CHECK_NEAR(normalizedAutocorrelation(noise(0, 40), 1, 0), highSmooth, 0.05);
    CHECK(normalizedAutocorrelation(noise(PsgEngine::kNoiseFollowsNote, 40), 1, 0)
          > normalizedAutocorrelation(noise(PsgEngine::kNoiseFollowsNote, 90), 1, 0));
}

TEST(psg_periodic_noise_plays_the_note_even_low_down)
{
    for (int note : { 28, 36, 45, 60 })
    {
        PsgParams params;
        params.noise = true;
        params.periodic = true;
        auto engine = makePsg(params);
        engine->noteOn(note, 127);
        render(*engine, 2400);
        const auto out = render(*engine, 96000);
        // 16 steps of the register, at the divider the chip can have for this note.
        const double divider = std::clamp(std::round(PsgEngine::kClock / (32.0 * 16.0 * noteFrequency(note))), 1.0, 1023.0);
        const double lag = 48000.0 * 32.0 * 16.0 * divider / PsgEngine::kClock;
        const double atPeriod = normalizedAutocorrelation(out, static_cast<size_t>(std::lround(lag)), 0);
        std::printf("  note %d: correlation %.3f at the period\n", note, atPeriod);
        CHECK(atPeriod > 0.8);
        CHECK(rms(out) > 0.02);
    }
}

TEST(psg_pitch_follows_bend_vibrato_and_glide)
{
    auto bent = makePsg();
    bent->noteOn(69, 127);
    bent->setPitchBend(2.0);
    render(*bent, 4800);
    CHECK(std::abs(centsBetween(measureFrequency(render(*bent, 48000), 48000.0), psgToneFrequency(71))) < 5.0);

    PsgParams wide;
    wide.vibratoRateHz = 5.0f;
    wide.vibratoDepthCents = 100.0f;
    auto vibrato = makePsg(wide);
    vibrato->noteOn(69, 127);
    render(*vibrato, 4800);
    double lowest = 1e9, highest = 0.0;
    for (int i = 0; i < 20; ++i)
    {
        const double f = measureFrequency(render(*vibrato, 960), 48000.0);
        lowest = std::min(lowest, f);
        highest = std::max(highest, f);
    }
    CHECK(centsBetween(highest, lowest) > 100.0);

    PsgParams glide;
    glide.glideMs = 200.0f;
    auto sliding = makePsg(glide);
    sliding->noteOn(57, 127);
    render(*sliding, 2400);
    sliding->noteOn(81, 127);
    sliding->noteOff(57);
    const double early = measureFrequency(render(*sliding, 2400), 48000.0);
    render(*sliding, 12000);
    CHECK(early < 600.0);
    CHECK(std::abs(centsBetween(measureFrequency(render(*sliding, 24000), 48000.0), psgToneFrequency(81))) < 5.0);
}

TEST(psg_ninth_note_steals_the_oldest)
{
    auto engine = makePsg();
    for (int i = 0; i < 9; ++i)
    {
        engine->noteOn(60 + i, 100);
        render(*engine, 48);
    }
    CHECK(engine->activeVoices() == 8);
    CHECK(!engine->isNoteActive(60));
    CHECK(engine->isNoteActive(68));
    CHECK(peak(render(*engine, 4800)) <= 1.1);
}

TEST(psg_sustain_pedal_holds_notes)
{
    auto engine = makePsg();
    engine->setSustain(true);
    engine->noteOn(69, 127);
    engine->noteOff(69);
    render(*engine, 24000);
    CHECK(engine->isNoteActive(69));
    CHECK(rms(render(*engine, 2400)) > 0.05);
    engine->setSustain(false);
    render(*engine, 9600);
    CHECK(engine->activeVoices() == 0);
}

TEST(psg_all_notes_off_and_channel_change_stop_notes)
{
    auto engine = makePsg();
    engine->noteOn(69, 127);
    render(*engine, 2400);
    engine->allNotesOff();
    CHECK(engine->activeVoices() == 0);
    render(*engine, 9600);
    CHECK(rms(render(*engine, 2400)) < 1e-3);

    engine->noteOn(69, 127);
    render(*engine, 480);
    PsgParams noise;
    noise.noise = true;
    engine->setParams(noise);
    CHECK(engine->activeVoices() == 0);

    // Other parameters leave notes alone.
    engine->noteOn(69, 127);
    noise.sustain = 8;
    noise.noiseRate = 1;
    noise.periodic = true;
    engine->setParams(noise);
    CHECK(engine->activeVoices() == 1);
}

TEST(psg_retrigger_restarts_the_envelope)
{
    PsgParams params;
    params.attackMs = 300.0f;
    auto engine = makePsg(params);
    engine->noteOn(69, 127);
    render(*engine, 24000);
    engine->noteOn(69, 127);
    CHECK(engine->activeVoices() == 1);
    CHECK(rms(render(*engine, 960)) < 0.05);
}

TEST(psg_invalid_events_are_ignored)
{
    auto unprepared = std::make_unique<PsgEngine>();
    unprepared->noteOn(60, 100);
    CHECK(unprepared->activeVoices() == 0);
    CHECK(peak(render(*unprepared, 480)) == 0.0);

    auto engine = makePsg();
    engine->noteOn(-1, 100);
    engine->noteOn(128, 100);
    CHECK(engine->activeVoices() == 0);
    engine->noteOn(60, 100);
    engine->noteOn(60, 0);
    render(*engine, 9600);
    CHECK(engine->activeVoices() == 0);
    engine->noteOff(61);

    PsgParams odd;
    odd.sustain = 99;
    odd.noiseRate = -4;
    odd.attackMs = -5.0f;
    odd.releaseMs = -5.0f;
    engine->setParams(odd);
    engine->noteOn(60, 100);
    const auto out = render(*engine, 2400);
    CHECK(std::all_of(out.begin(), out.end(), [](float s) { return std::isfinite(s); }));
    CHECK(rms(out) > 0.01);
}

TEST(psg_silent_after_prepare_with_no_dc)
{
    auto engine = makePsg();
    CHECK(peak(render(*engine, 4800)) == 0.0);

    // Periodic noise is high one step in 16, which the output must not keep as an offset.
    PsgParams params;
    params.noise = true;
    params.periodic = true;
    auto buzz = makePsg(params);
    buzz->noteOn(60, 127);
    render(*buzz, 96000);
    const auto out = render(*buzz, 48000);
    double mean = 0.0;
    for (const auto s : out)
        mean += s;
    CHECK(std::abs(mean / static_cast<double>(out.size())) < 2e-3);
}

TEST(psg_prepare_resets_voices)
{
    auto engine = makePsg();
    engine->noteOn(60, 100);
    render(*engine, 480);
    CHECK(engine->latencySamples() > 0);
    engine->prepare(44100.0);
    CHECK(engine->activeVoices() == 0);
    CHECK(peak(render(*engine, 480)) == 0.0);
}

TEST(psg_large_and_empty_blocks)
{
    auto engine = makePsg();
    engine->noteOn(69, 127);
    engine->render(nullptr, 0);
    const auto large = render(*engine, 200000);
    CHECK(std::abs(centsBetween(measureFrequency(large, 48000.0, 4800), psgToneFrequency(69))) < 3.0);
}

TEST(psg_audio_thread_calls_do_not_allocate)
{
    PsgParams params;
    params.vibratoDepthCents = 30.0f;
    params.glideMs = 100.0f;
    auto engine = makePsg(params);
    PsgParams noise = params;
    noise.noise = true;
    std::vector<float> block(30000);

    const auto before = allocationCount.load();
    for (int note = 50; note < 62; ++note)
    {
        engine->noteOn(note, 100);
        engine->render(block.data(), 64);
    }
    engine->setPitchBend(1.5);
    engine->render(block.data(), 30000);
    engine->setSustain(true);
    engine->noteOff(61);
    engine->setSustain(false);
    engine->noteOff(60);
    engine->setParams(noise);
    engine->noteOn(70, 90);
    engine->render(block.data(), 512);
    engine->allNotesOff();
    engine->render(block.data(), 512);
    const auto after = allocationCount.load();

    CHECK(after == before);
}

TEST(psg_8_voices_under_10_percent_cpu)
{
    auto engine = makePsg();
    for (int i = 0; i < 8; ++i)
        engine->noteOn(60 + i, 100);
    std::vector<float> block(512);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 94; ++i) // one second at 48 kHz
        engine->render(block.data(), 512);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("  %.1f %% of one core\n", seconds * 100.0);
    CHECK(seconds < 0.10);
}

// Writes .fti files the way FamiTracker does, so the reader can be checked against what it would read.
struct FtiWriter
{
    std::vector<std::uint8_t> bytes;

    explicit FtiWriter(int chip, const char* version = "2.4", const std::string& name = "Test")
    {
        for (const char c : std::string("FTI") + version)
            bytes.push_back(static_cast<std::uint8_t>(c));
        int8(chip);
        string(name);
    }
    void int8(int value) { bytes.push_back(static_cast<std::uint8_t>(value)); }
    void int32(std::int32_t value)
    {
        for (int i = 0; i < 4; ++i)
            bytes.push_back(static_cast<std::uint8_t>((static_cast<std::uint32_t>(value) >> (8 * i)) & 0xFF));
    }
    void string(const std::string& text)
    {
        int32(static_cast<std::int32_t>(text.size()));
        bytes.insert(bytes.end(), text.begin(), text.end());
    }
    // Five slots; an empty list is a slot that is off.
    void sequences(const std::array<std::vector<int>, 5>& values, const std::array<int, 5>& loops = { -1, -1, -1, -1, -1 },
                   const std::array<int, 5>& releases = { -1, -1, -1, -1, -1 }, const std::array<int, 5>& settings = { 0, 0, 0, 0, 0 })
    {
        int8(5);
        for (size_t i = 0; i < 5; ++i)
        {
            int8(values[i].empty() ? 0 : 1);
            if (values[i].empty())
                continue;
            int32(static_cast<std::int32_t>(values[i].size()));
            int32(loops[i]);
            int32(releases[i]);
            int32(settings[i]);
            for (const int v : values[i])
                int8(v);
        }
    }
};

TEST(fti_reads_a_2a03_instrument_with_its_samples)
{
    FtiWriter file(1, "2.4", "Kick and snare");
    file.sequences({ std::vector<int> { 15, 12, 8, 0 }, { 0, 12, -12 }, {}, {}, { 2, 1 } }, { -1, 1, -1, -1, -1 }, { 2, -1, -1, -1, -1 },
                   { 0, kArpeggioFixed, 0, 0, 0 });
    file.int32(2);                       // two keys
    file.int8(36); file.int8(1); file.int8(0x8F); file.int8(64); // C-3: sample slot 0, rate 15, loop, delta 64
    file.int8(38); file.int8(3); file.int8(0x0A); file.int8(-1); // D-3: slot 2, rate 10
    file.int32(2);                       // two samples
    file.int32(0); file.string("kick"); file.string(std::string("\x55\xAA\x0F", 3));
    file.int32(2); file.string("snare"); file.string(std::string(17, '\x33'));

    const auto parsed = parseFti(file.bytes.data(), file.bytes.size());
    CHECK(parsed.error == FtiError::none);
    if (!parsed.instrument)
        return;
    const auto& fti = *parsed.instrument;
    CHECK(fti.chip == FtiChip::Ricoh2A03);
    CHECK(fti.name == "Kick and snare");

    const auto& volume = fti.sequences[kFtiVolume];
    CHECK(volume.enabled && volume.values == std::vector<std::int8_t>({ 15, 12, 8, 0 }));
    CHECK(volume.loop == -1 && volume.release == 2);
    const auto& arpeggio = fti.sequences[kFtiArpeggio];
    CHECK(arpeggio.enabled && arpeggio.loop == 1 && arpeggio.setting == kArpeggioFixed);
    CHECK(arpeggio.values == std::vector<std::int8_t>({ 0, 12, -12 }));
    CHECK(!fti.sequences[kFtiPitch].enabled && !fti.sequences[kFtiHiPitch].enabled);
    CHECK(fti.sequences[kFtiDuty].values == std::vector<std::int8_t>({ 2, 1 }));

    CHECK(fti.samples.size() == 2);
    CHECK(fti.keys.size() == 2);
    if (fti.keys.size() != 2 || fti.samples.size() != 2)
        return;
    CHECK(fti.keys[0].note == 48 && fti.keys[0].rate == 15 && fti.keys[0].loop && fti.keys[0].delta == 64);
    CHECK(fti.samples[static_cast<size_t>(fti.keys[0].sample)].name == "kick");
    CHECK(fti.samples[static_cast<size_t>(fti.keys[0].sample)].bytes == std::vector<std::uint8_t>({ 0x55, 0xAA, 0x0F }));
    CHECK(fti.keys[1].note == 50 && fti.keys[1].rate == 10 && !fti.keys[1].loop && fti.keys[1].delta == -1);
    CHECK(fti.samples[static_cast<size_t>(fti.keys[1].sample)].name == "snare");
}

TEST(fti_reads_the_older_versions_fields_as_they_were)
{
    // 2.1 has no setting, and 2.3 no starting level for samples.
    FtiWriter old(1, "2.1");
    old.int8(5);
    old.int8(1); old.int32(2); old.int32(-1); old.int32(0); old.int8(9); old.int8(4);
    for (int i = 0; i < 4; ++i)
        old.int8(0);
    old.int32(0);
    old.int32(0);
    const auto parsed = parseFti(old.bytes.data(), old.bytes.size());
    CHECK(parsed.instrument.has_value());
    if (parsed.instrument)
    {
        CHECK(parsed.instrument->sequences[kFtiVolume].release == 0);
        CHECK(parsed.instrument->sequences[kFtiVolume].values == std::vector<std::int8_t>({ 9, 4 }));
    }

    FtiWriter noDelta(1, "2.3");
    noDelta.sequences({});
    noDelta.int32(1);
    noDelta.int8(24); noDelta.int8(1); noDelta.int8(3);
    noDelta.int32(1);
    noDelta.int32(0); noDelta.string("hit"); noDelta.string("abc");
    const auto three = parseFti(noDelta.bytes.data(), noDelta.bytes.size());
    CHECK(three.instrument.has_value() && three.instrument->keys.size() == 1 && three.instrument->keys[0].delta == -1);
}

TEST(fti_reads_a_vrc6_and_an_n163_instrument)
{
    FtiWriter vrc6(2, "2.4", "Saw lead");
    vrc6.sequences({ std::vector<int> { 63, 40 }, {}, { 1, -1 }, {}, { 7 } }, { -1, -1, 0, -1, -1 }, { -1, -1, -1, -1, -1 }, { kVolume64Steps, 0, 0, 0, 0 });
    const auto saw = parseFti(vrc6.bytes.data(), vrc6.bytes.size());
    CHECK(saw.instrument.has_value() && saw.instrument->chip == FtiChip::Vrc6);
    if (saw.instrument)
        CHECK(saw.instrument->sequences[kFtiVolume].setting == kVolume64Steps);

    FtiWriter n163(5, "2.4", "Waves");
    n163.sequences({ std::vector<int> { 15 }, {}, {}, {}, { 0, 1 } });
    n163.int32(8); // wave size
    n163.int32(0); // wave position
    n163.int32(2); // wave count
    for (int wave = 0; wave < 2; ++wave)
        for (int i = 0; i < 8; ++i)
            n163.int8(wave == 0 ? i * 2 : 15 - i);
    const auto waves = parseFti(n163.bytes.data(), n163.bytes.size());
    CHECK(waves.instrument.has_value());
    if (!waves.instrument)
        return;
    CHECK(waves.instrument->chip == FtiChip::N163);
    CHECK(waves.instrument->waves.size() == 2);
    CHECK(waves.instrument->waves[0] == std::vector<std::uint8_t>({ 0, 2, 4, 6, 8, 10, 12, 14 }));
    CHECK(waves.instrument->waves[1].size() == 8 && waves.instrument->waves[1][0] == 15);

    // 2.5 adds a field before the count.
    FtiWriter newer(5, "2.5");
    newer.sequences({});
    newer.int32(4); newer.int32(0); newer.int32(1); newer.int32(1);
    for (int i = 0; i < 4; ++i)
        newer.int8(i);
    const auto five = parseFti(newer.bytes.data(), newer.bytes.size());
    CHECK(five.instrument.has_value() && five.instrument->waves.size() == 1);
}

TEST(fti_refuses_what_it_cannot_play)
{
    const auto errorOf = [](const std::vector<std::uint8_t>& bytes) { return parseFti(bytes.data(), bytes.size()).error; };

    CHECK(parseFti(nullptr, 0).error == FtiError::notFti);
    CHECK(errorOf({ 'T', 'F', 'I', '2', '.', '4', 1 }) == FtiError::notFti);
    for (const int chip : { 3, 4, 6 })
    {
        FtiWriter other(chip);
        other.sequences({});
        CHECK(errorOf(other.bytes) == FtiError::unsupportedChip);
    }
    FtiWriter ancient(1, "1.9");
    CHECK(errorOf(ancient.bytes) == FtiError::unsupportedVersion);
    FtiWriter future(1, "3.0");
    CHECK(errorOf(future.bytes) == FtiError::unsupportedVersion);
    FtiWriter nonsense(9);
    CHECK(errorOf(nonsense.bytes) == FtiError::unsupportedChip);

    // Cut anywhere, a good file is damaged, never read past its end.
    FtiWriter good(1);
    good.sequences({ std::vector<int> { 15, 10 }, { 0 }, {}, {}, {} });
    good.int32(1);
    good.int8(36); good.int8(1); good.int8(15); good.int8(0);
    good.int32(1);
    good.int32(0); good.string("s"); good.string("abc");
    CHECK(errorOf(good.bytes) == FtiError::none);
    for (size_t length = 7; length < good.bytes.size(); ++length)
    {
        const std::vector<std::uint8_t> cut(good.bytes.begin(), good.bytes.begin() + static_cast<std::ptrdiff_t>(length));
        CHECK(errorOf(cut) != FtiError::none);
    }

    // Values out of their range.
    FtiWriter tooLong(1);
    tooLong.int8(5);
    tooLong.int8(1); tooLong.int32(300); tooLong.int32(-1); tooLong.int32(-1); tooLong.int32(0);
    CHECK(errorOf(tooLong.bytes) == FtiError::damaged);
    FtiWriter badLoop(1);
    badLoop.sequences({ std::vector<int> { 1, 2 }, {}, {}, {}, {} }, { 5, -1, -1, -1, -1 });
    badLoop.int32(0); badLoop.int32(0);
    CHECK(errorOf(badLoop.bytes) == FtiError::damaged);
    FtiWriter badWave(5);
    badWave.sequences({});
    badWave.int32(8); badWave.int32(0); badWave.int32(1);
    for (int i = 0; i < 8; ++i)
        badWave.int8(i == 3 ? 16 : 1);
    CHECK(errorOf(badWave.bytes) == FtiError::damaged);
}

TEST(fti_leaves_out_keys_it_cannot_place)
{
    FtiWriter file(1);
    file.sequences({});
    file.int32(3);
    file.int8(10); file.int8(1); file.int8(15); file.int8(-1);  // MIDI 22: kept
    file.int8(120); file.int8(1); file.int8(15); file.int8(-1); // MIDI 132: off the keyboard
    file.int8(12); file.int8(5); file.int8(15); file.int8(-1);  // a sample that isn't there
    file.int32(1);
    file.int32(0); file.string("s"); file.string("x");
    const auto parsed = parseFti(file.bytes.data(), file.bytes.size());
    CHECK(parsed.instrument.has_value() && parsed.instrument->keys.size() == 1 && parsed.instrument->keys[0].note == 22);
}

std::vector<int> playSequence(const FtiSequence& sequence, int steps, int releaseAt = -1)
{
    SequenceCursor cursor;
    cursor.reset();
    std::vector<int> out;
    for (int i = 0; i < steps; ++i)
    {
        if (i == releaseAt)
            cursor.release(sequence);
        int value = -99;
        CHECK(cursor.step(sequence, value));
        out.push_back(value);
    }
    return out;
}

FtiSequence sequenceOf(std::vector<int> values, int loop = -1, int release = -1)
{
    FtiSequence sequence;
    sequence.enabled = true;
    for (const int v : values)
        sequence.values.push_back(static_cast<std::int8_t>(v));
    sequence.loop = loop;
    sequence.release = release;
    return sequence;
}

TEST(sequence_plays_holds_and_loops)
{
    CHECK(playSequence(sequenceOf({ 1, 2, 3 }), 6) == std::vector<int>({ 1, 2, 3, 3, 3, 3 }));
    CHECK(playSequence(sequenceOf({ 1, 2, 3 }, 1), 7) == std::vector<int>({ 1, 2, 3, 2, 3, 2, 3 }));

    SequenceCursor cursor;
    cursor.reset();
    int value = 0;
    const auto held = sequenceOf({ 5, 6 });
    cursor.step(held, value);
    CHECK(!cursor.finished());
    cursor.step(held, value);
    cursor.step(held, value);
    CHECK(cursor.finished());

    FtiSequence off;
    CHECK(!cursor.step(off, value));
    FtiSequence empty;
    empty.enabled = true;
    CHECK(!cursor.step(empty, value));
}

TEST(sequence_waits_at_its_release_point_until_the_key_is_up)
{
    // Attack, hold on the release point, then the tail.
    const auto envelope = sequenceOf({ 15, 12, 10, 6, 3, 0 }, -1, 2);
    CHECK(playSequence(envelope, 6) == std::vector<int>({ 15, 12, 10, 10, 10, 10 }));
    CHECK(playSequence(envelope, 8, 4) == std::vector<int>({ 15, 12, 10, 10, 6, 3, 0, 0 }));

    // A loop before the release point goes round while the key is down.
    const auto tremolo = sequenceOf({ 15, 8, 12, 4, 0 }, 1, 2);
    CHECK(playSequence(tremolo, 7) == std::vector<int>({ 15, 8, 12, 8, 12, 8, 12 }));
    CHECK(playSequence(tremolo, 6, 3) == std::vector<int>({ 15, 8, 12, 4, 0, 0 }));

    // A loop after it goes round once the key is up.
    const auto echo = sequenceOf({ 15, 10, 6, 3 }, 2, 1);
    CHECK(playSequence(echo, 8, 2) == std::vector<int>({ 15, 10, 6, 3, 6, 3, 6, 3 }));

    // Released past the end, it holds its last value.
    const auto shortOne = sequenceOf({ 9, 7 }, -1, 1);
    CHECK(playSequence(shortOne, 4, 1) == std::vector<int>({ 9, 7, 7, 7 }));
}

double dpcmDecodedCorrelation(const std::vector<std::uint8_t>& bytes, const std::vector<float>& input)
{
    int level = dpcm::kMiddleLevel;
    double sum = 0.0, a = 0.0, b = 0.0;
    for (size_t i = 0; i < input.size() && i / 8 < bytes.size(); ++i)
    {
        level = dpcm::nextLevel(level, ((bytes[i / 8] >> (i % 8)) & 1) != 0);
        const double decoded = level - dpcm::kMiddleLevel;
        sum += decoded * input[i];
        a += decoded * decoded;
        b += static_cast<double>(input[i]) * input[i];
    }
    return a > 0.0 && b > 0.0 ? sum / std::sqrt(a * b) : 0.0;
}

TEST(dpcm_rates_are_the_chips)
{
    CHECK_NEAR(dpcm::rateHz(0), 1789773.0 / 428.0, 0.01);
    CHECK_NEAR(dpcm::rateHz(15), 1789773.0 / 54.0, 0.01);
    for (int rate = 0; rate < 16; ++rate)
        CHECK(dpcm::nearestRate(dpcm::rateHz(rate) * 1.01) == rate);
    CHECK(dpcm::nearestRate(1.0e6) == 15);
    CHECK(dpcm::nearestRate(10.0) == 0);
    CHECK(dpcm::nextLevel(126, true) == 126);
    CHECK(dpcm::nextLevel(125, true) == 127);
    CHECK(dpcm::nextLevel(1, false) == 1);
    CHECK(dpcm::nextLevel(2, false) == 0);
}

TEST(dpcm_encoding_follows_the_signal_and_fits_the_chip)
{
    std::vector<float> sine(4000);
    for (size_t i = 0; i < sine.size(); ++i)
        sine[i] = 0.8f * static_cast<float>(std::sin(2.0 * kPi * 300.0 * static_cast<double>(i) / dpcm::rateHz(15)));
    const auto bytes = dpcm::encode(sine);
    CHECK(bytes.size() % 16 == 1);
    CHECK(bytes.size() * 8 >= sine.size());
    CHECK(dpcmDecodedCorrelation(bytes, sine) > 0.95);

    // The padding goes up and down, so the level stays where the signal left it.
    std::vector<float> one(20, 0.0f);
    const auto padded = dpcm::encode(one);
    CHECK(padded.size() == 17);
    for (size_t i = 3; i < padded.size(); ++i)
        CHECK(padded[i] == 0x55 || padded[i] == 0xAA);

    std::vector<float> tooLong(static_cast<size_t>(dpcm::kMaxBytes) * 8 + 5000, 0.1f);
    CHECK(static_cast<int>(dpcm::encode(tooLong).size()) == dpcm::kMaxBytes);
    CHECK(dpcm::encode({}).empty());
}

TEST(n163_waves_are_4_bits_and_distinct)
{
    const auto& waves = n163Waves();
    for (size_t i = 0; i < waves.size(); ++i)
    {
        CHECK(static_cast<int>(waves[i].size()) == kN163WaveLength);
        for (const auto sample : waves[i])
            CHECK(sample <= 15);
        const auto [low, high] = std::minmax_element(waves[i].begin(), waves[i].end());
        CHECK(*low <= 1 && *high >= 14); // each uses the chip's whole range
        for (size_t j = 0; j < i; ++j)
            CHECK(waves[i] != waves[j]);
    }
}

std::vector<float> sineAtDpcmRate(double frequency, double seconds)
{
    std::vector<float> samples(static_cast<size_t>(seconds * dpcm::rateHz(15)));
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = 0.8f * static_cast<float>(std::sin(2.0 * kPi * frequency * static_cast<double>(i) / dpcm::rateHz(15)));
    return samples;
}

NesEngine makeNesWith(const NesParams& params, const NesUserSound* sound)
{
    NesEngine engine;
    engine.setParams(params);
    engine.prepare(48000.0);
    engine.setUserSound(sound);
    return engine;
}

// The strongest of a few candidate frequencies, as a way to read the pitch of a noisy signal.
double strongestOf(const std::vector<float>& out, std::initializer_list<double> candidates)
{
    double best = 0.0, bestMagnitude = -1.0;
    for (const double f : candidates)
    {
        const double m = magnitudeAt(out, f, 48000.0);
        if (m > bestMagnitude)
        {
            bestMagnitude = m;
            best = f;
        }
    }
    return best;
}

NesUserSound dpcmSound(double frequency = 440.0, double seconds = 0.9)
{
    NesUserSound sound;
    sound.dpcm = DpcmSample { dpcm::encode(sineAtDpcmRate(frequency, seconds)), "tone" };
    return sound;
}

NesParams dpcmParams(int root = 69, bool loop = false)
{
    NesParams params;
    params.channel = NesChannel::Dpcm;
    params.dpcmRootNote = root;
    params.dpcmLoop = loop;
    return params;
}

TEST(nes_dpcm_plays_the_sample_at_the_chips_rates)
{
    const auto sound = dpcmSound();
    const auto played = [&](int note)
    {
        auto engine = makeNesWith(dpcmParams(), &sound);
        engine.noteOn(note, 127);
        return render(engine, 24000);
    };

    // The root note plays it as recorded; others take the nearest of the 16 rates, and none is faster than the last.
    const auto expected = [](int note)
    {
        const int rate = dpcm::nearestRate(dpcm::rateHz(15) * std::pow(2.0, (note - 69) / 12.0));
        return 440.0 * dpcm::rateHz(rate) / dpcm::rateHz(15);
    };
    for (int note : { 69, 57, 50, 81 })
    {
        const auto out = played(note);
        CHECK(rms(out) > 0.02);
        const double want = expected(note);
        CHECK(strongestOf(out, { want, want * 1.06, want / 1.06, want * 1.5, want / 1.5 }) == want);
    }
    CHECK(expected(81) == 440.0);
}

TEST(nes_dpcm_plays_to_its_end_or_loops_while_held)
{
    const auto sound = dpcmSound(440.0, 0.2);
    auto once = makeNesWith(dpcmParams(), &sound);
    once.noteOn(69, 127);
    render(once, 2400);
    once.noteOff(69);
    CHECK(once.activeVoices() == 1); // the key doesn't stop it
    render(once, 9600);
    CHECK(once.activeVoices() == 0);

    auto looped = makeNesWith(dpcmParams(69, true), &sound);
    looped.noteOn(69, 127);
    render(looped, 48000);
    CHECK(looped.activeVoices() == 1);
    CHECK(rms(render(looped, 2400)) > 0.02);
    looped.noteOff(69);
    CHECK(looped.activeVoices() == 0);
}

TEST(nes_dpcm_has_no_volume)
{
    const auto sound = dpcmSound();
    const auto level = [&](int velocity, float sustain)
    {
        auto params = dpcmParams();
        params.sustain = sustain;
        auto engine = makeNesWith(params, &sound);
        engine.noteOn(69, velocity);
        return rms(render(engine, 12000));
    };
    CHECK_NEAR(level(1, 15.0f), level(127, 15.0f), 1e-6);
    CHECK_NEAR(level(127, 0.0f), level(127, 15.0f), 1e-6);
    CHECK(level(127, 15.0f) > 0.05); // as loud as the channel is on the console, above a pulse
}

TEST(nes_dpcm_without_a_sample_is_silent)
{
    NesUserSound none;
    auto engine = makeNesWith(dpcmParams(), &none);
    engine.noteOn(69, 127);
    CHECK(engine.activeVoices() == 0);
    auto unset = makeNesWith(dpcmParams(), nullptr);
    unset.noteOn(69, 127);
    CHECK(unset.activeVoices() == 0);
}

NesUserSound kitSound()
{
    NesUserSound sound;
    FtiInstrument fti;
    fti.samples.push_back({ dpcm::encode(sineAtDpcmRate(440.0, 0.9)), "tone" });
    fti.keys.push_back({ 48, 0, 15, false, -1 });
    fti.keys.push_back({ 50, 0, 7, true, 64 });
    sound.instruments[0] = fti;
    return sound;
}

TEST(nes_dpcm_kit_plays_each_key_at_its_rate)
{
    const auto sound = kitSound();
    auto params = dpcmParams();
    params.useInstrument = true;
    const auto played = [&](int note)
    {
        auto engine = makeNesWith(params, &sound);
        engine.noteOn(note, 127);
        return render(engine, 24000);
    };
    const double slow = 440.0 * dpcm::rateHz(7) / dpcm::rateHz(15);
    CHECK(strongestOf(played(48), { 440.0, slow, 220.0 }) == 440.0);
    CHECK(strongestOf(played(50), { 440.0, slow, 220.0 }) == slow);

    auto engine = makeNesWith(params, &sound);
    engine.noteOn(49, 127);
    CHECK(engine.activeVoices() == 0);

    // The loop is the key's, not the parameter's.
    engine.noteOn(50, 127);
    render(engine, 96000);
    CHECK(engine.activeVoices() == 1);

    // Without the instrument, the kit is not used.
    params.useInstrument = false;
    auto plain = makeNesWith(params, &sound);
    plain.noteOn(48, 127);
    CHECK(plain.activeVoices() == 0);
}

NesParams chipParams(NesChip chip)
{
    NesParams params;
    params.chip = chip;
    return params;
}

TEST(vrc6_pulse_is_in_tune_with_its_8_widths)
{
    for (int note : { 45, 69, 84 })
    {
        auto engine = makeNesWith(chipParams(NesChip::Vrc6), nullptr);
        engine.noteOn(note, 127);
        const double timer = std::round(nes::kCpuClock / (16.0 * noteFrequency(note))) - 1.0;
        CHECK(std::abs(centsBetween(measureFrequency(render(engine, 48000), 48000.0, 4800), nes::kCpuClock / (16.0 * (timer + 1.0)))) < 3.0);
    }

    // At 8 steps in 16 the wave is a square, with no second harmonic; at 4, a 25 % pulse, which has one.
    const auto secondHarmonic = [](int duty)
    {
        auto params = chipParams(NesChip::Vrc6);
        params.vrc6Duty = duty;
        auto engine = makeNesWith(params, nullptr);
        engine.noteOn(57, 127);
        const auto out = render(engine, 48000);
        return magnitudeAt(out, 2.0 * 220.0, 48000.0) / magnitudeAt(out, 220.0, 48000.0);
    };
    CHECK(secondHarmonic(7) < 0.05);
    CHECK(secondHarmonic(3) > 0.4);
}

TEST(vrc6_pulse_matches_the_2a03s_level)
{
    const auto level = [](NesChip chip)
    {
        auto engine = makeNesWith(chipParams(chip), nullptr);
        engine.noteOn(69, 127);
        render(engine, 2400);
        return rms(render(engine, 24000));
    };
    CHECK_NEAR(decibels(level(NesChip::Vrc6) / level(NesChip::Ricoh2A03)), 0.0, 1.0);
}

TEST(vrc6_saw_is_in_tune_and_louder)
{
    auto params = chipParams(NesChip::Vrc6);
    params.vrc6Saw = true;
    for (int note : { 45, 69, 84 })
    {
        auto engine = makeNesWith(params, nullptr);
        engine.noteOn(note, 127);
        const double timer = std::round(nes::kCpuClock / (14.0 * noteFrequency(note))) - 1.0;
        CHECK(std::abs(centsBetween(measureFrequency(render(engine, 48000), 48000.0, 4800), nes::kCpuClock / (14.0 * (timer + 1.0)))) < 3.0);
    }
    auto engine = makeNesWith(params, nullptr);
    engine.noteOn(57, 127);
    const auto out = render(engine, 48000);
    CHECK(magnitudeAt(out, 2.0 * 220.0, 48000.0) > 0.3 * magnitudeAt(out, 220.0, 48000.0)); // a saw has every harmonic

    auto pulse = makeNesWith(chipParams(NesChip::Vrc6), nullptr);
    pulse.noteOn(57, 127);
    CHECK(rms(out) > rms(render(pulse, 48000)));

    // The volume sets the saw's rate, so a quiet saw is quieter.
    params.sustain = 5.0f;
    auto quiet = makeNesWith(params, nullptr);
    quiet.noteOn(57, 127);
    render(quiet, 4800);
    CHECK(rms(render(quiet, 24000)) < 0.5 * rms(out));
}

TEST(n163_plays_its_wave_in_tune)
{
    for (int note : { 45, 69, 84 })
    {
        auto engine = makeNesWith(chipParams(NesChip::N163), nullptr);
        engine.noteOn(note, 127);
        CHECK(std::abs(centsBetween(measureFrequency(render(engine, 48000), 48000.0, 4800), noteFrequency(note))) < 3.0);
    }

    // The sine has little third harmonic; the square has a strong one.
    const auto third = [](int wave)
    {
        auto params = chipParams(NesChip::N163);
        params.n163Wave = wave;
        auto engine = makeNesWith(params, nullptr);
        engine.noteOn(57, 127);
        const auto out = render(engine, 48000);
        return magnitudeAt(out, 3.0 * 220.0, 48000.0) / magnitudeAt(out, 220.0, 48000.0);
    };
    CHECK(third(0) < 0.1);
    CHECK(third(3) > 0.2);
}

TEST(n163_runs_coarser_with_more_channels)
{
    // Each channel's update rate drops with their number, which puts what isn't the note between its harmonics.
    const auto betweenHarmonics = [](int channels)
    {
        auto params = chipParams(NesChip::N163);
        params.n163Channels = channels;
        auto engine = makeNesWith(params, nullptr);
        engine.noteOn(84, 127);
        render(engine, 2400);
        const auto out = render(engine, 48000);
        const double fundamental = noteFrequency(84);
        double energy = 0.0;
        for (double f = 100.0; f < 20000.0; f += 23.0)
        {
            const double nearest = std::round(f / fundamental) * fundamental;
            if (std::abs(f - nearest) > 60.0)
                energy += std::pow(magnitudeAt(out, f, 48000.0), 2.0);
        }
        return energy;
    };
    const double one = betweenHarmonics(1), eight = betweenHarmonics(8);
    std::printf("  between harmonics: %.3g with 1 channel, %.3g with 8\n", one, eight);
    CHECK(eight > 2.0 * one);
}

TEST(n163_level_is_close_to_a_pulse)
{
    const auto level = [](NesChip chip, int wave)
    {
        auto params = chipParams(chip);
        params.n163Wave = wave;
        auto engine = makeNesWith(params, nullptr);
        engine.noteOn(69, 127);
        render(engine, 2400);
        return rms(render(engine, 24000));
    };
    CHECK_NEAR(decibels(level(NesChip::N163, 3) / level(NesChip::Ricoh2A03, 0)), 0.0, 3.0);
}

NesUserSound instrumentSound(NesChip chip, const std::vector<std::vector<int>>& values, int volumeRelease = -1, std::array<int, 5> settings = {})
{
    NesUserSound sound;
    FtiInstrument fti;
    fti.chip = chip == NesChip::Ricoh2A03 ? FtiChip::Ricoh2A03 : chip == NesChip::Vrc6 ? FtiChip::Vrc6 : FtiChip::N163;
    for (size_t i = 0; i < 5; ++i)
    {
        fti.sequences[i] = sequenceOf(values[i]);
        fti.sequences[i].setting = settings[i];
        fti.sequences[i].enabled = !values[i].empty();
    }
    fti.sequences[kFtiVolume].release = volumeRelease;
    sound.instruments[static_cast<size_t>(chip)] = fti;
    return sound;
}

NesParams withInstrument(NesParams params)
{
    params.useInstrument = true;
    return params;
}

TEST(fti_volume_sequence_shapes_the_note_and_ends_it)
{
    // Full for a quarter second, then silence: the note ends by itself.
    std::vector<int> volume(15, 15);
    volume.push_back(0);
    const auto sound = instrumentSound(NesChip::Ricoh2A03, { volume, {}, {}, {}, {} });
    auto engine = makeNesWith(withInstrument({}), &sound);
    engine.noteOn(69, 127);
    CHECK(rms(render(engine, 4800)) > 0.05);
    render(engine, 9600);
    CHECK(engine.activeVoices() == 0);

    // The envelope's knobs don't apply to it.
    auto params = withInstrument({});
    params.sustain = 0.0f;
    params.decayMs = 10.0f;
    const auto held = instrumentSound(NesChip::Ricoh2A03, { std::vector<int> { 15 }, {}, {}, {}, {} });
    auto steady = makeNesWith(params, &held);
    steady.noteOn(69, 127);
    render(steady, 24000);
    CHECK(rms(render(steady, 2400)) > 0.05);
}

TEST(fti_release_point_and_key_up)
{
    // Held on the release point, then down the tail once the key is up.
    const auto sound = instrumentSound(NesChip::Ricoh2A03, { std::vector<int> { 15, 15, 8, 8, 8, 0 }, {}, {}, {}, {} }, 1);
    auto engine = makeNesWith(withInstrument({}), &sound);
    engine.noteOn(69, 127);
    render(engine, 24000);
    const double heldLevel = rms(render(engine, 2400));
    CHECK(heldLevel > 0.05);
    engine.noteOff(69);
    CHECK(engine.activeVoices() == 1);
    render(engine, 1600);
    CHECK(rms(render(engine, 800)) < heldLevel);
    render(engine, 4800);
    CHECK(engine.activeVoices() == 0);

    // With no release point, the key cuts the note, as FamiTracker's note off does.
    const auto noRelease = instrumentSound(NesChip::Ricoh2A03, { std::vector<int> { 15 }, {}, {}, {}, {} });
    auto cut = makeNesWith(withInstrument({}), &noRelease);
    cut.noteOn(69, 127);
    render(cut, 2400);
    cut.noteOff(69);
    CHECK(cut.activeVoices() == 0);
}

TEST(fti_arpeggio_moves_the_note)
{
    const auto measured = [](const NesUserSound& sound, int note)
    {
        auto engine = makeNesWith(withInstrument({}), &sound);
        engine.noteOn(note, 127);
        render(engine, 2400);
        return measureFrequency(render(engine, 48000), 48000.0);
    };
    const auto up = instrumentSound(NesChip::Ricoh2A03, { {}, std::vector<int> { 12 }, {}, {}, {} });
    CHECK(std::abs(centsBetween(measured(up, 57), pulseFrequency(69))) < 5.0);

    // A fixed arpeggio plays its own note, FamiTracker's A-4 being 57 from C-0.
    const auto fixed = instrumentSound(NesChip::Ricoh2A03, { {}, std::vector<int> { 57 }, {}, {}, {} }, -1, { 0, kArpeggioFixed, 0, 0, 0 });
    CHECK(std::abs(centsBetween(measured(fixed, 40), pulseFrequency(69))) < 5.0);
    CHECK(std::abs(centsBetween(measured(fixed, 80), pulseFrequency(69))) < 5.0);
}

TEST(fti_pitch_sequence_bends_the_period)
{
    // +1 on the period at every step: the note keeps going down.
    const auto sound = instrumentSound(NesChip::Ricoh2A03, { {}, {}, std::vector<int> { 1 }, {}, {} });
    auto engine = makeNesWith(withInstrument({}), &sound);
    engine.noteOn(69, 127);
    const double early = measureFrequency(render(engine, 4800), 48000.0);
    render(engine, 19200);
    const double late = measureFrequency(render(engine, 4800), 48000.0);
    CHECK(late < early * 0.95);
}

TEST(fti_duty_sequence_sets_the_width_and_the_wave)
{
    const auto second = [](const NesUserSound* sound, NesParams params)
    {
        auto engine = makeNesWith(params, sound);
        engine.noteOn(57, 127);
        render(engine, 2400);
        const auto out = render(engine, 48000);
        return magnitudeAt(out, 440.0, 48000.0) / magnitudeAt(out, 220.0, 48000.0);
    };
    // The 2A03's default is a square; the sequence makes it 25 %.
    const auto quarter = instrumentSound(NesChip::Ricoh2A03, { {}, {}, {}, {}, std::vector<int> { 1 } });
    CHECK(second(nullptr, {}) < 0.05);
    CHECK(second(&quarter, withInstrument({})) > 0.4);

    // On the N163 it picks one of the instrument's waves.
    NesUserSound waves = instrumentSound(NesChip::N163, { {}, {}, {}, {}, std::vector<int> { 1 } });
    auto& fti = *waves.instruments[static_cast<size_t>(NesChip::N163)];
    fti.waves.push_back(n163Waves()[0]); // a sine
    fti.waves.push_back(n163Waves()[4]); // a 25 % pulse
    CHECK(second(&waves, withInstrument(chipParams(NesChip::N163))) > 0.4);
    fti.sequences[kFtiDuty].values = { 0 };
    CHECK(second(&waves, withInstrument(chipParams(NesChip::N163))) < 0.1);
}

TEST(nes_chip_or_instrument_changes_stop_notes)
{
    const auto sound = instrumentSound(NesChip::Ricoh2A03, { std::vector<int> { 15 }, {}, {}, {}, {} });
    auto engine = makeNesWith({}, &sound);
    engine.noteOn(60, 127);
    engine.setParams(chipParams(NesChip::Vrc6));
    CHECK(engine.activeVoices() == 0);
    engine.noteOn(60, 127);
    engine.setParams(withInstrument(chipParams(NesChip::Vrc6)));
    CHECK(engine.activeVoices() == 0);
    engine.noteOn(60, 127);
    auto other = sound;
    engine.setUserSound(&other);
    CHECK(engine.activeVoices() == 0);

    // Other parameters leave notes alone.
    engine.noteOn(60, 127);
    auto wider = withInstrument(chipParams(NesChip::Vrc6));
    wider.vrc6Duty = 2;
    engine.setParams(wider);
    CHECK(engine.activeVoices() == 1);
}

TEST(nes_new_sounds_do_not_allocate)
{
    auto sound = kitSound();
    sound.dpcm = DpcmSample { dpcm::encode(sineAtDpcmRate(440.0, 0.1)), "tone" };
    std::vector<float> block(4096);
    NesEngine engine;
    engine.prepare(48000.0);
    engine.setUserSound(&sound);
    NesParams params;
    const NesParams settings[] = { dpcmParams(), withInstrument(dpcmParams()), chipParams(NesChip::Vrc6), chipParams(NesChip::N163) };

    const auto before = allocationCount.load();
    for (const auto& next : settings)
    {
        params = next;
        engine.setParams(params);
        for (int note = 46; note < 52; ++note)
        {
            engine.noteOn(note, 100);
            engine.render(block.data(), 512);
        }
        engine.render(block.data(), 4096);
        engine.allNotesOff();
    }
    const auto after = allocationCount.load();
    CHECK(after == before);
}

int main()
{
    for (const auto& test : registry())
    {
        std::printf("%s\n", test.name);
        test.run();
    }
    std::printf("%zu tests, %d failed checks\n", registry().size(), failures);
    return failures == 0 ? 0 : 1;
}
