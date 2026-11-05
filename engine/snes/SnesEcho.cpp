// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "SnesEcho.h"

#include <algorithm>
#include <cmath>

namespace retrochip
{
namespace
{
// From fullsnes, oldest sample first. They add up to 127, and the sum is divided by 128.
constexpr std::array<float, SnesEcho::kTaps> kCoefficients { 4, 10, 16, 20, 23, 22, 18, 14 };
constexpr float kCoefficientScale = 1.0f / 128.0f;

// The filter's heaviest tap is the fifth, three samples before the newest. The line is shortened
// by that much, so the echo's weight lands on the delay that was set, and so does each repeat.
constexpr int kFilterCentre = 3;

// Keeps a long decay from ending in denormals, which are slow on some CPUs.
constexpr float kFloor = 1e-18f;
} // namespace

int SnesEcho::samplesFor(int count) const
{
    // Whole samples for the whole delay, not for a step, so the rounding does not add up.
    return count == 0 ? 0 : static_cast<int>(std::lround(count * kStepSeconds * rate)) - kFilterCentre;
}

void SnesEcho::prepare(double sampleRate)
{
    if (!(sampleRate > 0.0))
        return;

    rate = sampleRate;
    const int size = static_cast<int>(std::lround(kMaxSteps * kStepSeconds * rate));
    buffer.assign(static_cast<size_t>(std::max(size, kFilterCentre + 1)), 0.0f);
    history.fill(0.0f);
    writeIndex = 0;
    length = samplesFor(steps);
}

void SnesEcho::setDelay(int newSteps)
{
    steps = std::clamp(newSteps, 0, kMaxSteps);
    const int newLength = samplesFor(steps);
    if (newLength == length)
        return;
    const int size = static_cast<int>(buffer.size());

    // The samples the longer line reads that it did not hold: written before it was as long.
    for (int age = length + 1; age <= newLength; ++age)
    {
        int index = writeIndex - age;
        if (index < 0)
            index += size;
        buffer[static_cast<size_t>(index)] = 0.0f;
    }
    if (newLength == 0)
        history.fill(0.0f);
    length = newLength;
}

void SnesEcho::process(float* inOut, int n, float mix, float feedback)
{
    if (length == 0 || n <= 0)
        return;

    const int size = static_cast<int>(buffer.size());
    mix = std::clamp(mix, 0.0f, 1.0f);
    feedback = std::clamp(feedback, 0.0f, 1.0f);

    for (int i = 0; i < n; ++i)
    {
        int readIndex = writeIndex - length;
        if (readIndex < 0)
            readIndex += size;

        for (size_t k = 0; k + 1 < history.size(); ++k)
            history[k] = history[k + 1];
        history.back() = buffer[static_cast<size_t>(readIndex)];

        float filtered = 0.0f;
        for (size_t k = 0; k < history.size(); ++k)
            filtered += kCoefficients[k] * history[k];
        filtered *= kCoefficientScale;

        const float dry = inOut[i];
        float written = dry + filtered * feedback;
        if (std::abs(written) < kFloor)
            written = 0.0f;
        buffer[static_cast<size_t>(writeIndex)] = written;
        inOut[i] = dry + filtered * mix;

        if (++writeIndex == size)
            writeIndex = 0;
    }
}

void SnesEcho::reset()
{
    std::fill(buffer.begin(), buffer.end(), 0.0f);
    history.fill(0.0f);
    writeIndex = 0;
}
} // namespace retrochip
