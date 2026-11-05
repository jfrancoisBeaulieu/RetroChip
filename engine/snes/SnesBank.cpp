// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "SnesBank.h"

#include "BrrEncoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace retrochip
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 32000.0;

// A small generator of our own, so no instrument depends on any other or on global state.
class Rng
{
public:
    explicit Rng(std::uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) { next(); }

    std::uint64_t next()
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    }

    double uniform() { return static_cast<double>(next() >> 11) / 9007199254740992.0; }  // 0 to 1
    double bipolar() { return 2.0 * uniform() - 1.0; }                                   // -1 to 1
    double phase() { return 2.0 * kPi * uniform(); }
    int between(int low, int high) { return low + static_cast<int>(next() % static_cast<std::uint64_t>(high - low + 1)); }

private:
    std::uint64_t state;
};

// One sine, with its own way of arriving and fading. All times are in samples.
struct Partial
{
    double frequency = 0.0; // in cycles per sample
    double amplitude = 1.0;
    double phase = 0.0;
    double delay = 0.0; // silent until then
    double rise = 0.0;  // smooth rise to full over this long, once the delay is over
    double tau = 0.0;   // exponential fade towards the floor, 0 for none
    double floor = 0.0; // what the fade leaves
};

double envelopeAt(const Partial& p, double t)
{
    double e = 1.0;
    if (p.rise > 0.0)
    {
        const double x = std::clamp((t - p.delay) / p.rise, 0.0, 1.0);
        e = x * x * (3.0 - 2.0 * x);
    }
    else if (t < p.delay)
    {
        e = 0.0;
    }
    if (p.tau > 0.0)
        e *= p.floor + (1.0 - p.floor) * std::exp(-t / p.tau);
    return e;
}

// Adds the partials over [0, size). From settleAt on, the amplitudes stay as they were then (the
// run-up is over, and the loop that follows has to repeat), so the loop is the same every cycle
// as long as every frequency makes a whole number of cycles in it.
void addPartials(std::vector<double>& signal, const std::vector<Partial>& partials, double settleAt)
{
    const auto size = signal.size();
    for (const auto& p : partials)
    {
        const double settled = p.amplitude * envelopeAt(p, settleAt);
        const double w = 2.0 * kPi * p.frequency;
        for (size_t n = 0; n < size; ++n)
        {
            const double t = static_cast<double>(n);
            const double a = t >= settleAt ? settled : p.amplitude * envelopeAt(p, t);
            signal[n] += a * std::sin(w * t + p.phase);
        }
    }
}

// Low-passed noise, as a tool for breath, the strike of a mallet or a pick. window gives the
// level at position 0 to 1 over the length.
template <typename Window>
void addNoise(std::vector<double>& signal, Rng& rng, size_t length, double level, double smoothing, Window window)
{
    double lowpassed = 0.0;
    for (size_t n = 0; n < std::min(length, signal.size()); ++n)
    {
        lowpassed += smoothing * (rng.bipolar() - lowpassed);
        signal[n] += level * lowpassed * window(static_cast<double>(n) / static_cast<double>(length));
    }
}

double hill(double x) { return std::sin(kPi * x) * std::sin(kPi * x); } // 0 at both ends
double fall(double x) { return std::exp(-6.0 * x) * (1.0 - x); }       // starts at 1, 0 at the end

// A loop of a whole number of cycles at the root note: the length is a multiple of 16 samples, at
// least minLength, whichever of the next 400 such lengths puts the nearest whole cycle count
// closest to the note's pitch.
struct LoopShape
{
    int length = 0;
    int cycles = 0; // of the root, in the loop
    double centsError = 0.0;
};

double noteFrequency(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

LoopShape findLoop(int rootNote, int minLength)
{
    const double wanted = noteFrequency(rootNote);
    LoopShape best;
    double bestError = 1e9;
    for (int length = minLength; length < minLength + 400 * 16; length += 16)
    {
        const int cycles = std::max(1, static_cast<int>(std::lround(length * wanted / kRate)));
        const double cents = 1200.0 * std::log2(cycles * kRate / length / wanted);
        if (std::abs(cents) < bestError - 1e-9)
        {
            bestError = std::abs(cents);
            best = { length, cycles, cents };
        }
    }
    return best;
}

// The signal for an instrument, until it becomes a sample.
struct Voiced
{
    std::vector<double> signal;
    int loopStart = -1; // sample, a multiple of 16
};

std::shared_ptr<const SnesSample> finish(const char* name, int rootNote, Voiced voiced)
{
    auto& s = voiced.signal;
    double peak = 0.0;
    for (double v : s)
        peak = std::max(peak, std::abs(v));

    // Sustained sounds are set to one level of the loop, decaying ones by their peak, and nothing
    // goes past 0.9 of the range.
    double scale = 0.9 / std::max(peak, 1e-9);
    if (voiced.loopStart >= 0)
    {
        double sum = 0.0;
        for (size_t n = static_cast<size_t>(voiced.loopStart); n < s.size(); ++n)
            sum += s[n] * s[n];
        const double rms = std::sqrt(sum / static_cast<double>(s.size() - static_cast<size_t>(voiced.loopStart)));
        scale = std::min(scale, 0.2 / std::max(rms, 1e-9));
    }

    std::vector<float> pcm(s.size());
    for (size_t n = 0; n < s.size(); ++n)
        pcm[n] = static_cast<float>(s[n] * scale);

    auto sample = std::make_shared<SnesSample>();
    sample->brr = encodeBrr(pcm, voiced.loopStart);
    sample->loopBlock = brrLoopBlockFor(voiced.loopStart);
    sample->rootNote = rootNote;
    sample->name = name;
    return sample;
}

// A sustained instrument: a run-up of `attack` samples, then the loop. The frequency of a
// partial is its cycles over the loop's length.
Voiced sustain(const LoopShape& loop, int attack, std::vector<Partial> partials)
{
    Voiced v;
    v.signal.assign(static_cast<size_t>(attack + loop.length), 0.0);
    v.loopStart = attack;
    addPartials(v.signal, partials, attack);
    return v;
}

double cyclesToFrequency(const LoopShape& loop, double cycles) { return cycles / loop.length; }

// Fades the last `length` samples to silence, with a cosine.
void fadeOut(std::vector<double>& signal, size_t length)
{
    const size_t start = signal.size() - length;
    for (size_t n = start; n < signal.size(); ++n)
    {
        const double x = static_cast<double>(n - start) / static_cast<double>(length);
        signal[n] *= 0.5 + 0.5 * std::cos(kPi * x);
    }
}

// ---- The instruments ----------------------------------------------------------------------

// Bowed strings: a sawtooth-like spectrum, three detuned players, and a run-up where the upper
// harmonics come in after the fundamental.
std::shared_ptr<const SnesSample> cordes()
{
    constexpr int root = 57, attack = 2560;
    Rng rng(101);
    const auto loop = findLoop(root, 16384);
    std::vector<Partial> partials;
    const std::array<std::pair<int, double>, 3> players { { { -1, 0.6 }, { 0, 1.0 }, { 1, 0.6 } } };
    for (const auto& [offset, level] : players)
        for (int k = 1; k <= 18; ++k)
        {
            Partial p;
            p.frequency = cyclesToFrequency(loop, k * (loop.cycles + offset));
            p.amplitude = level * std::exp(-k / 14.0) / k;
            p.phase = rng.phase();
            p.delay = std::min((k - 1) * 40.0, 0.4 * attack);
            p.rise = 0.6 * attack;
            partials.push_back(p);
        }
    return finish("Strings", root, sustain(loop, attack, partials));
}

// Brass: the upper harmonics arrive late, which is the blare of the attack.
std::shared_ptr<const SnesSample> cuivres()
{
    constexpr int root = 58, attack = 3200;
    Rng rng(202);
    const auto loop = findLoop(root, 2048);
    std::vector<Partial> partials;
    for (int k = 1; k <= 16; ++k)
    {
        Partial p;
        p.frequency = cyclesToFrequency(loop, k * loop.cycles);
        p.amplitude = std::pow(k, -0.7) * (1.0 + 0.4 * std::sin(1.3 * k));
        p.phase = 0.3 * rng.bipolar();
        p.delay = (k - 1) * 100.0;
        p.rise = 1600.0;
        partials.push_back(p);
    }
    return finish("Brass", root, sustain(loop, attack, partials));
}

// Organ drawbars: steady, no run-up.
std::shared_ptr<const SnesSample> orgue()
{
    constexpr int root = 48;
    Rng rng(303);
    const auto loop = findLoop(root, 1024);
    const std::array<std::pair<int, double>, 7> drawbars { { { 1, 1.0 }, { 2, 0.7 }, { 3, 0.45 }, { 4, 0.5 }, { 5, 0.12 }, { 6, 0.25 }, { 8, 0.2 } } };
    std::vector<Partial> partials;
    for (const auto& [k, level] : drawbars)
    {
        Partial p;
        p.frequency = cyclesToFrequency(loop, k * loop.cycles);
        p.amplitude = level;
        p.phase = rng.phase();
        partials.push_back(p);
    }
    return finish("Organ", root, sustain(loop, 0, partials));
}

// A plucked bass that settles: the upper harmonics begin strong and fall to a lower sustain.
std::shared_ptr<const SnesSample> basse()
{
    constexpr int root = 36, attack = 2400;
    Rng rng(404);
    const auto loop = findLoop(root, 2048);
    std::vector<Partial> partials;
    for (int k = 1; k <= 10; ++k)
    {
        Partial p;
        p.frequency = cyclesToFrequency(loop, k * loop.cycles);
        p.amplitude = std::pow(k, -0.6);
        p.phase = rng.phase();
        if (k >= 2)
        {
            p.tau = 960.0 / (1.0 + 0.1 * k);
            p.floor = k == 2 ? 0.6 : 0.25;
        }
        partials.push_back(p);
    }
    return finish("Bass", root, sustain(loop, attack, partials));
}

// A bell: inharmonic partials, each fading at its own rate, and a short strike. Rings out, no loop.
std::shared_ptr<const SnesSample> cloche()
{
    constexpr int root = 72, length = 64000;
    Rng rng(505);
    const double f0 = noteFrequency(root) / kRate;
    struct Mode { double ratio, level, seconds; };
    const std::array<Mode, 10> modes { { { 0.5, 0.35, 2.5 }, { 1.0, 1.0, 1.8 }, { 1.183, 0.6, 1.4 }, { 1.506, 0.5, 1.1 }, { 2.0, 0.45, 0.9 },
                                          { 2.514, 0.3, 0.7 }, { 2.662, 0.25, 0.55 }, { 3.2, 0.2, 0.4 }, { 4.1, 0.12, 0.3 }, { 5.43, 0.07, 0.2 } } };
    std::vector<Partial> partials;
    for (const auto& m : modes)
    {
        Partial p;
        p.frequency = m.ratio * f0;
        p.amplitude = m.level;
        p.phase = rng.phase();
        p.rise = 16.0;
        p.tau = m.seconds * kRate;
        partials.push_back(p);
    }
    Voiced v;
    v.signal.assign(length, 0.0);
    addPartials(v.signal, partials, 1e18);
    addNoise(v.signal, rng, 100, 0.5, 0.5, fall);
    fadeOut(v.signal, 8000);
    return finish("Bell", root, std::move(v));
}

// A flute: a strong fundamental, weak harmonics, breath in the run-up and a thin steady breath.
std::shared_ptr<const SnesSample> flute()
{
    constexpr int root = 72, attack = 3200;
    Rng rng(606);
    const auto loop = findLoop(root, 4096);
    std::vector<Partial> partials;
    const std::array<double, 4> levels { 1.0, 0.18, 0.06, 0.03 };
    for (int k = 1; k <= 4; ++k)
    {
        Partial p;
        p.frequency = cyclesToFrequency(loop, k * loop.cycles);
        p.amplitude = levels[static_cast<size_t>(k - 1)];
        p.phase = rng.phase();
        p.delay = (k - 1) * 400.0;
        p.rise = 1600.0;
        partials.push_back(p);
    }
    // Breath: sines at random whole cycles between 2 and 6 kHz, which repeat with the loop.
    for (int i = 0; i < 48; ++i)
    {
        Partial p;
        p.frequency = cyclesToFrequency(loop, rng.between(loop.length / 16, loop.length * 3 / 16));
        p.amplitude = 0.012;
        p.phase = rng.phase();
        p.rise = 800.0;
        partials.push_back(p);
    }
    auto v = sustain(loop, attack, partials);
    addNoise(v.signal, rng, attack, 0.5, 0.35, hill); // the chiff of the attack, gone by the loop
    return finish("Flute", root, std::move(v));
}

// A choir on "ah": three detuned voices under formants at about 700, 1150 and 2600 Hz, and a slow run-up.
std::shared_ptr<const SnesSample> choeur()
{
    constexpr int root = 55, attack = 6400;
    Rng rng(707);
    const auto loop = findLoop(root, 16384);
    std::vector<Partial> partials;
    const std::array<std::pair<int, double>, 3> voices { { { -1, 0.7 }, { 0, 1.0 }, { 1, 0.7 } } };
    for (const auto& [offset, level] : voices)
        for (int k = 1; k <= 24; ++k)
        {
            const int cycles = k * (loop.cycles + offset);
            const double hz = cycles * kRate / loop.length;
            const auto bump = [&](double centre, double width) { return std::exp(-0.5 * std::pow((hz - centre) / width, 2.0)); };
            Partial p;
            p.frequency = cyclesToFrequency(loop, cycles);
            p.amplitude = level * (bump(700, 300) + 0.6 * bump(1150, 250) + 0.3 * bump(2600, 500) + 0.04 / k);
            p.phase = rng.phase();
            p.rise = attack;
            partials.push_back(p);
        }
    return finish("Choir", root, sustain(loop, attack, partials));
}

// A struck string: harmonics a little stretched, faded by order, two strings a hair apart, a hammer thump.
std::shared_ptr<const SnesSample> piano()
{
    constexpr int root = 60, length = 72000;
    Rng rng(808);
    const double f0 = noteFrequency(root) / kRate;
    std::vector<Partial> partials;
    for (int k = 1; k <= 24; ++k)
    {
        const double stretch = std::sqrt(1.0 + 0.0004 * k * k);
        const double level = std::pow(k, -0.9) * (0.3 + 0.7 * std::abs(std::sin(kPi * k * 0.12)));
        const double seconds = 2.0 / (1.0 + 0.25 * (k - 1));
        for (int string = 0; string < 2; ++string)
        {
            Partial p;
            p.frequency = k * f0 * stretch * (string == 0 ? 1.0 : 1.0006);
            p.amplitude = level * (string == 0 ? 1.0 : 0.6);
            p.phase = rng.phase();
            p.rise = 8.0;
            p.tau = seconds * kRate * (string == 0 ? 0.45 : 1.1);
            partials.push_back(p);
        }
    }
    Voiced v;
    v.signal.assign(length, 0.0);
    addPartials(v.signal, partials, 1e18);
    addNoise(v.signal, rng, 200, 0.8, 0.25, fall);
    fadeOut(v.signal, 12000);
    return finish("Piano", root, std::move(v));
}

// A plucked guitar string: the plucking point leaves its mark on the harmonics, and the high ones die quickly.
std::shared_ptr<const SnesSample> guitare()
{
    constexpr int root = 52, length = 48000;
    Rng rng(909);
    const double f0 = noteFrequency(root) / kRate;
    std::vector<Partial> partials;
    for (int k = 1; k <= 22; ++k)
    {
        Partial p;
        p.frequency = k * f0 * std::sqrt(1.0 + 0.00005 * k * k);
        p.amplitude = (0.25 + std::abs(std::sin(kPi * k * 0.16))) / k;
        p.phase = rng.phase();
        p.rise = 6.0;
        p.tau = 1.4 / (1.0 + 0.45 * (k - 1)) * kRate;
        partials.push_back(p);
    }
    Voiced v;
    v.signal.assign(length, 0.0);
    addPartials(v.signal, partials, 1e18);
    addNoise(v.signal, rng, 130, 0.35, 0.4, fall);
    fadeOut(v.signal, 8000);
    return finish("Guitar", root, std::move(v));
}

// A marimba: the bar's partials at about 1, 4 and 10 times the pitch, the upper ones gone almost at once.
std::shared_ptr<const SnesSample> marimba()
{
    constexpr int root = 64, length = 32000;
    Rng rng(1010);
    const double f0 = noteFrequency(root) / kRate;
    struct Mode { double ratio, level, seconds; };
    const std::array<Mode, 3> modes { { { 1.0, 1.0, 0.35 }, { 3.97, 0.4, 0.12 }, { 9.9, 0.12, 0.05 } } };
    std::vector<Partial> partials;
    for (const auto& m : modes)
    {
        Partial p;
        p.frequency = m.ratio * f0;
        p.amplitude = m.level;
        p.phase = rng.phase();
        p.rise = 6.0;
        p.tau = m.seconds * kRate;
        partials.push_back(p);
    }
    Voiced v;
    v.signal.assign(length, 0.0);
    addPartials(v.signal, partials, 1e18);
    addNoise(v.signal, rng, 70, 0.4, 0.3, fall);
    fadeOut(v.signal, 6400);
    return finish("Marimba", root, std::move(v));
}

// A reed: odd harmonics, a hollow tone, with a little buzz and a quick run-up.
std::shared_ptr<const SnesSample> anche()
{
    constexpr int root = 57, attack = 1600;
    Rng rng(1111);
    const auto loop = findLoop(root, 4096);
    std::vector<Partial> partials;
    for (int k = 1; k <= 13; ++k)
    {
        Partial p;
        p.frequency = cyclesToFrequency(loop, k * loop.cycles);
        const double level = k % 2 == 1 ? std::pow(k, -0.8) : 0.05 / k;
        p.amplitude = level * (1.0 + 1.2 * std::exp(-0.5 * std::pow((k - 5.0) / 1.5, 2.0)));
        p.phase = rng.phase();
        p.delay = (k - 1) * 60.0;
        p.rise = 800.0;
        partials.push_back(p);
    }
    for (int i = 0; i < 24; ++i)
    {
        Partial p;
        p.frequency = cyclesToFrequency(loop, rng.between(loop.length / 16, loop.length / 8));
        p.amplitude = 0.01;
        p.phase = rng.phase();
        p.rise = 800.0;
        partials.push_back(p);
    }
    return finish("Reed", root, sustain(loop, attack, partials));
}

// A pad: soft harmonics from three players a few cycles apart, a fifth above, a long slow run-up.
std::shared_ptr<const SnesSample> nappe()
{
    constexpr int root = 48, attack = 12800;
    Rng rng(1212);
    const auto loop = findLoop(root, 32768);
    std::vector<Partial> partials;
    const std::array<std::pair<int, double>, 3> players { { { -2, 0.8 }, { 0, 1.0 }, { 2, 0.8 } } };
    for (const auto& [offset, level] : players)
    {
        for (int k = 1; k <= 10; ++k)
        {
            Partial p;
            p.frequency = cyclesToFrequency(loop, k * (loop.cycles + offset));
            p.amplitude = level * std::pow(k, -1.5);
            p.phase = rng.phase();
            p.delay = k * 400.0;
            p.rise = 6000.0;
            partials.push_back(p);
        }
        Partial fifth; // 3/2 of the pitch, to the nearest whole cycle
        fifth.frequency = cyclesToFrequency(loop, std::lround(1.5 * (loop.cycles + offset)));
        fifth.amplitude = level * 0.35;
        fifth.phase = rng.phase();
        fifth.delay = 3000.0;
        fifth.rise = 6000.0;
        partials.push_back(fifth);
    }
    return finish("Pad", root, sustain(loop, attack, partials));
}
} // namespace

std::vector<std::shared_ptr<const SnesSample>> buildSnesBank()
{
    return { cordes(), cuivres(), orgue(), basse(), cloche(), flute(), choeur(), piano(), guitare(), marimba(), anche(), nappe() };
}
} // namespace retrochip
