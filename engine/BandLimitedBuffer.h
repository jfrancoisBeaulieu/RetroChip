// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include <array>

namespace retrochip
{
// Collects level changes as band-limited steps, so square edges don't alias.
class BandLimitedBuffer
{
public:
    static constexpr int kMaxChunk = 8192;
    static constexpr int kTaps = 24;
    static constexpr int kPhases = 64;
    static constexpr int kLatencySamples = 12;

    BandLimitedBuffer();

    // samplePos must be in [0, kMaxChunk).
    void addStep(double samplePos, float delta);

    // Writes the next numSamples (at most kMaxChunk) and advances.
    void read(float* out, int numSamples);

    void clear();

private:
    using Kernel = std::array<std::array<float, kTaps>, kPhases>;

    const Kernel* kernel;
    std::array<float, kMaxChunk + kTaps + 1> deltas {};
    double level = 0.0;
};
} // namespace retrochip
