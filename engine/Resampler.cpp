// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "Resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace retrochip
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kCutoffFactor = 0.45;
constexpr int kHalfTaps = Resampler::kTaps / 2;

double sinc(double x)
{
    if (std::abs(x) < 1e-12)
        return 1.0;
    return std::sin(kPi * x) / (kPi * x);
}

double blackman(double d)
{
    const double t = d / kHalfTaps; // -1..1
    return 0.42 + 0.5 * std::cos(kPi * t) + 0.08 * std::cos(2.0 * kPi * t);
}
} // namespace

Resampler::Resampler() { clearQueue(); }

void Resampler::prepare(double inputRate, double outputRate)
{
    prepared = false;
    clearQueue();
    if (!(inputRate > 0.0) || !(outputRate > 0.0))
        return;

    step = inputRate / outputRate;
    latency = static_cast<int>(std::lround(kHalfTaps / step));

    // Cutoff in cycles per input sample.
    const double cutoff = kCutoffFactor * std::min(inputRate, outputRate) / inputRate;
    for (int phase = 0; phase <= kPhases; ++phase)
    {
        const double fraction = static_cast<double>(phase) / kPhases;
        std::array<double, kTaps> taps {};
        double sum = 0.0;
        for (int k = 0; k < kTaps; ++k)
        {
            // Tap k reads the sample (kHalfTaps - 1 - k) before the window centre's floor.
            const double d = fraction + (kHalfTaps - 1 - k);
            taps[static_cast<size_t>(k)] = 2.0 * cutoff * sinc(2.0 * cutoff * d) * blackman(d);
            sum += taps[static_cast<size_t>(k)];
        }
        for (int k = 0; k < kTaps; ++k) // unity gain at DC for every phase
            table[static_cast<size_t>(phase)][static_cast<size_t>(k)] = static_cast<float>(taps[static_cast<size_t>(k)] / sum);
    }
    prepared = true;
}

void Resampler::reset() { clearQueue(); }

void Resampler::clearQueue()
{
    // kHistory zeros precede the first real sample. The first window is centred
    // kHalfTaps before it, so the output lags the input by exactly that much.
    queue.fill(0.0f);
    count = kHistory;
    position = kHalfTaps - 1;
}

int Resampler::inputNeededFor(int outputSamples) const
{
    if (!prepared || outputSamples <= 0)
        return 0;
    const double lastCentre = position + (outputSamples - 1) * step;
    // The window for a centre c needs queue indices up to floor(c) + kHalfTaps. One extra
    // sample absorbs rounding differences with pull()'s running position.
    const double needed = std::floor(lastCentre) + kHalfTaps + 2.0 - count;
    return needed > 0.0 ? std::min(static_cast<int>(needed), kCapacity - count) : 0;
}

void Resampler::push(const float* in, int numIn)
{
    if (!prepared || in == nullptr || numIn <= 0)
        return;
    const int n = std::min(numIn, kCapacity - count);
    if (n <= 0)
        return;
    std::memcpy(queue.data() + count, in, static_cast<size_t>(n) * sizeof(float));
    count += n;
}

int Resampler::pull(float* out, int maxOut)
{
    if (!prepared || out == nullptr || maxOut <= 0)
        return 0;

    int produced = 0;
    while (produced < maxOut)
    {
        const int base = static_cast<int>(position);
        if (base + kHalfTaps >= count)
            break;

        const double phasePos = (position - base) * kPhases;
        const int phase = static_cast<int>(phasePos);
        const float blend = static_cast<float>(phasePos - phase);
        const float* a = table[static_cast<size_t>(phase)].data();
        const float* b = table[static_cast<size_t>(phase) + 1].data();
        const float* x = queue.data() + base - (kHalfTaps - 1);

        float sum = 0.0f;
        for (int k = 0; k < kTaps; ++k)
            sum += x[k] * (a[k] + blend * (b[k] - a[k]));
        out[produced++] = sum;
        position += step;
    }

    // Drop what the next window no longer reads.
    const int drop = static_cast<int>(position) - (kHalfTaps - 1);
    if (drop > 0)
    {
        std::memmove(queue.data(), queue.data() + drop, static_cast<size_t>(count - drop) * sizeof(float));
        count -= drop;
        position -= drop;
    }
    return produced;
}

int Resampler::latencyOutputSamples() const { return latency; }
} // namespace retrochip
