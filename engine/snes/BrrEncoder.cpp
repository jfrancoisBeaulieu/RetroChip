// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "BrrEncoder.h"

#include "Brr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace retrochip
{
namespace
{
constexpr int kMaxShift = 12;
constexpr int kFilters = 4;
constexpr double kFullScale = 0x3FF8;

struct Candidate
{
    double error = std::numeric_limits<double>::infinity();
    int shift = 0;
    int filter = 0;
    std::array<int, brr::kSamplesPerBlock> nibbles {};
};

int toTarget(float value)
{
    if (std::isnan(value))
        return 0;
    const double clipped = std::clamp(static_cast<double>(value), -1.0, 1.0);
    return std::clamp(static_cast<int>(std::lround(clipped * kFullScale)), brr::kEncodeMin, brr::kEncodeMax);
}

bool inRange(int decoded) { return decoded >= brr::kEncodeMin && decoded <= brr::kEncodeMax; }

// The nibble whose decoded sample is nearest the target and within range, or false if none is.
bool bestNibble(int target, int shift, int filter, int old, int older, int& nibbleOut, int& decodedOut)
{
    // Nearest first guess from the prediction alone (a nibble of 0), then its neighbours. Each is
    // checked through the decoder, which is where the clamping and wrapping happen.
    const int predicted = brr::decodeSample(0, shift, filter, old, older);
    const double step = std::ldexp(1.0, shift - 1); // one nibble step at this shift
    const int guess = static_cast<int>(std::lround((target - predicted) / step));

    int bestError = std::numeric_limits<int>::max();
    const auto consider = [&](int nibble)
    {
        const int decoded = brr::decodeSample(nibble, shift, filter, old, older);
        if (!inRange(decoded))
            return;
        const int error = std::abs(decoded - target);
        if (error < bestError)
        {
            bestError = error;
            nibbleOut = nibble;
            decodedOut = decoded;
        }
    };

    for (int nibble = guess - 1; nibble <= guess + 1; ++nibble)
        consider(std::clamp(nibble, -8, 7));
    if (bestError == std::numeric_limits<int>::max())
        for (int nibble = -8; nibble <= 7; ++nibble) // overflow around the prediction: look at all
            consider(nibble);
    return bestError != std::numeric_limits<int>::max();
}

Candidate tryEncoding(const std::array<int, brr::kSamplesPerBlock>& targets, int shift, int filter, int old, int older)
{
    Candidate candidate;
    candidate.shift = shift;
    candidate.filter = filter;
    double error = 0.0;
    for (size_t i = 0; i < targets.size(); ++i)
    {
        int nibble = 0, decoded = 0;
        if (!bestNibble(targets[i], shift, filter, old, older, nibble, decoded))
            return candidate; // no nibble keeps this combination in range: error stays infinite
        candidate.nibbles[i] = nibble;
        const double e = decoded - targets[i];
        error += e * e;
        older = old;
        old = decoded;
    }
    candidate.error = error;
    return candidate;
}
} // namespace

std::vector<std::uint8_t> encodeBrr(const std::vector<float>& pcm, int loopStartSample)
{
    std::vector<std::uint8_t> out;
    if (pcm.empty())
        return out;

    const size_t blocks = (pcm.size() + brr::kSamplesPerBlock - 1) / brr::kSamplesPerBlock;
    int loopBlock = brrLoopBlockFor(loopStartSample);
    if (loopBlock >= static_cast<int>(blocks))
        loopBlock = -1;
    out.reserve(blocks * brr::kBytesPerBlock);

    int old = 0;
    int older = 0;
    for (size_t block = 0; block < blocks; ++block)
    {
        std::array<int, brr::kSamplesPerBlock> targets {};
        for (size_t i = 0; i < targets.size(); ++i)
        {
            const size_t at = block * brr::kSamplesPerBlock + i;
            targets[i] = at < pcm.size() ? toTarget(pcm[at]) : 0;
        }

        // Filter 0 does not look at the history, which is why the first block and the loop block
        // use it: the decoder arrives there with the end of the sample, or with nothing.
        const bool historyFree = block == 0 || static_cast<int>(block) == loopBlock;
        Candidate best;
        for (int shift = 0; shift <= kMaxShift; ++shift)
        {
            for (int filter = 0; filter < (historyFree ? 1 : kFilters); ++filter)
            {
                const auto candidate = tryEncoding(targets, shift, filter, old, older);
                if (candidate.error < best.error)
                    best = candidate;
            }
        }
        // Filter 0 with zeros decodes in range at every shift, so something was found.

        const bool last = block + 1 == blocks;
        const int flags = last ? (loopBlock >= 0 ? 3 : 1) : 0;
        const size_t start = out.size();
        out.push_back(static_cast<std::uint8_t>((best.shift << 4) | (best.filter << 2) | flags));
        for (size_t i = 0; i < best.nibbles.size(); i += 2)
            out.push_back(static_cast<std::uint8_t>(((best.nibbles[i] & 0x0F) << 4) | (best.nibbles[i + 1] & 0x0F)));

        // Carry on from what the decoder itself makes of the block.
        std::int16_t decoded[brr::kSamplesPerBlock];
        brr::decodeBlock(out.data() + start, old, older, decoded);
    }
    return out;
}
} // namespace retrochip
