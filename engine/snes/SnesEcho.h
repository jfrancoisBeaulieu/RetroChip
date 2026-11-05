// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include <array>
#include <vector>

namespace retrochip
{
// The S-DSP's echo, mono: a delay line of 0 to 15 steps of 16 ms, an 8-point filter with the chip's
// fixed coefficients on the way out, and feedback into the line. At the chip's 32 kHz a step is
// 512 samples, and that is what the echo does until prepare() is called.
//
// The line is allocated at construction for 32 kHz and by prepare() for another rate, never on the
// audio thread. setDelay(), process() and reset() are safe on it. prepare() is not. Not shared
// between threads.
class SnesEcho
{
public:
    static constexpr int kStepSamples = 512;
    static constexpr int kMaxSteps = 15;
    static constexpr int kBufferSize = 7680; // kMaxSteps * kStepSamples: the line at 32 kHz
    static constexpr double kStepSeconds = 0.016;
    static constexpr int kTaps = 8;

    // Sizes the line for 240 ms at the host's rate and empties it. A step stays 16 ms, so the delay
    // in samples is steps x 16 ms x rate; the filter and its centre of 3 samples are the same at
    // every rate. The delay setting stays. Not for the audio thread: it allocates. A rate of 0 or
    // less changes nothing.
    void prepare(double sampleRate);

    // 0 bypasses the echo. Lengthening the delay clears the part of the line that comes into use,
    // so nothing from before the shorter delay is played again; what the line already holds stays.
    void setDelay(int steps);

    // Adds the echo to n samples in place: out = in + filtered x mix, and the line takes
    // in + filtered x feedback. Both are held to 0 to 1, where a feedback of 1 is the chip's gain
    // of 1 before the filter, whose own gain of 127/128 at DC makes it die away.
    void process(float* inOut, int n, float mix, float feedback);

    // Empties the line and the filter. The delay setting stays.
    void reset();

private:
    // The line's length in samples for a number of steps at the current rate.
    int samplesFor(int count) const;

    std::vector<float> buffer = std::vector<float>(kBufferSize, 0.0f);
    std::array<float, kTaps> history {}; // the last outputs of the line, oldest first
    double rate = 32000.0;
    int steps = 0; // the delay setting, 0 to kMaxSteps
    int writeIndex = 0;
    int length = 0; // samples between writing and reading the line; 0 when bypassed
};
} // namespace retrochip
