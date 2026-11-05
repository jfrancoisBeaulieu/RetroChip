// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include <array>

namespace retrochip
{
// Converts a chip's internal rate to the DAW rate. Blackman-windowed sinc with linear
// interpolation between kPhases sub-sample filters; the cutoff sits at 0.45 x the lower rate.
// Holds about 200 KB, so keep instances off the stack. Nothing allocates after construction.
class Resampler
{
public:
    static constexpr int kMaxChunk = 8192;
    static constexpr int kTaps = 48;
    static constexpr int kPhases = 256;

    Resampler();

    // Rates must be positive; otherwise the resampler stays unprepared and produces nothing.
    void prepare(double inputRate, double outputRate);
    void reset();

    // Input samples still to be pushed before pull() can deliver outputSamples. May be more
    // than strictly necessary. Never more than the queue has room for, so for a very large
    // request pull what is available and ask again; following that loop never overflows.
    int inputNeededFor(int outputSamples) const;

    // Queues input (at most kMaxChunk samples per call). Input beyond the queue's capacity
    // (about 4 x kMaxChunk samples waiting) is dropped, which breaks signal continuity;
    // pushing no more than inputNeededFor() reports avoids it.
    void push(const float* in, int numIn);

    // Writes up to maxOut samples and returns how many were produced. Input that wasn't
    // needed yet stays queued.
    int pull(float* out, int maxOut);

    // Delay through the filter, in output samples.
    int latencyOutputSamples() const;

private:
    static constexpr int kHistory = kTaps - 1;                      // zeros ahead of the first sample
    static constexpr int kCapacity = 4 * kMaxChunk + 2 * kTaps;

    void clearQueue();

    std::array<std::array<float, kTaps>, kPhases + 1> table {};
    std::array<float, kCapacity> queue {};
    int count = 0;
    double position = 0.0; // window centre in queue indices
    double step = 0.0;     // input samples per output sample
    int latency = 0;
    bool prepared = false;
};
} // namespace retrochip
