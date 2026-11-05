// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "BandLimitedBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace retrochip
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kCutoff = 0.45; // relative to the sample rate

template <typename Kernel>
Kernel buildKernel()
{
    Kernel kernel {};
    constexpr int taps = BandLimitedBuffer::kTaps;
    constexpr int phases = BandLimitedBuffer::kPhases;
    const double halfWidth = taps / 2.0;

    for (int phase = 0; phase < phases; ++phase)
    {
        const double centre = (taps / 2 - 1) + static_cast<double>(phase) / phases;
        double sum = 0.0;
        std::array<double, taps> values {};

        for (int tap = 0; tap < taps; ++tap)
        {
            const double x = tap - centre;
            const double arg = 2.0 * kCutoff * x;
            const double sinc = std::abs(arg) < 1e-9 ? 1.0 : std::sin(kPi * arg) / (kPi * arg);
            const double u = std::clamp(x / halfWidth, -1.0, 1.0);
            const double blackman = 0.42 + 0.5 * std::cos(kPi * u) + 0.08 * std::cos(2.0 * kPi * u);
            values[static_cast<size_t>(tap)] = sinc * blackman;
            sum += sinc * blackman;
        }

        for (int tap = 0; tap < taps; ++tap)
            kernel[static_cast<size_t>(phase)][static_cast<size_t>(tap)] =
                static_cast<float>(values[static_cast<size_t>(tap)] / sum);
    }
    return kernel;
}

template <typename Kernel>
const Kernel& sharedKernel()
{
    static const Kernel kernel = buildKernel<Kernel>();
    return kernel;
}
} // namespace

BandLimitedBuffer::BandLimitedBuffer() : kernel(&sharedKernel<Kernel>()) {}

void BandLimitedBuffer::addStep(double samplePos, float delta)
{
    const int index = static_cast<int>(samplePos);
    const int phase = std::min(static_cast<int>((samplePos - index) * kPhases), kPhases - 1);
    const auto& taps = (*kernel)[static_cast<size_t>(phase)];
    float* target = deltas.data() + index;

    for (int tap = 0; tap < kTaps; ++tap)
        target[tap] += delta * taps[static_cast<size_t>(tap)];
}

void BandLimitedBuffer::read(float* out, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        level += deltas[static_cast<size_t>(i)];
        out[i] = static_cast<float>(level);
    }

    constexpr int tail = kTaps + 1;
    std::memmove(deltas.data(), deltas.data() + numSamples, tail * sizeof(float));
    std::fill(deltas.data() + tail, deltas.data() + tail + numSamples, 0.0f);
}

void BandLimitedBuffer::clear()
{
    deltas.fill(0.0f);
    level = 0.0;
}
} // namespace retrochip
