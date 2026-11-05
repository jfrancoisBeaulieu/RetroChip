// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "Dpcm.h"

#include "NesTables.h"

#include <algorithm>
#include <cmath>

namespace retrochip::dpcm
{
namespace
{
constexpr double kHalfRange = 63.0;
constexpr std::uint8_t kAlternatingBits = 0x55;
} // namespace

double rateHz(int rate)
{
    return nes::kCpuClock / kRatePeriods[static_cast<std::size_t>(std::clamp(rate, 0, kFastestRate))];
}

int nearestRate(double hz)
{
    int best = 0;
    double bestDistance = 1e9;
    for (int rate = 0; rate <= kFastestRate; ++rate)
    {
        const double distance = std::abs(std::log(std::max(hz, 1e-6) / rateHz(rate)));
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = rate;
        }
    }
    return best;
}

std::vector<std::uint8_t> encode(const std::vector<float>& samples)
{
    if (samples.empty())
        return {};

    const std::size_t bits = std::min(samples.size(), static_cast<std::size_t>(kMaxBytes) * 8);
    std::vector<std::uint8_t> bytes((bits + 7) / 8, 0);
    int level = kMiddleLevel;
    for (std::size_t i = 0; i < bits; ++i)
    {
        const double target = kMiddleLevel + kHalfRange * std::clamp(static_cast<double>(samples[i]), -1.0, 1.0);
        const bool up = target > level;
        level = nextLevel(level, up);
        if (up)
            bytes[i / 8] = static_cast<std::uint8_t>(bytes[i / 8] | (1u << (i % 8)));
    }

    // The rest of the last byte, then whole bytes, go up and down in turn.
    for (std::size_t i = bits; i % 8 != 0; ++i)
        if (i % 2 == 0)
            bytes[i / 8] = static_cast<std::uint8_t>(bytes[i / 8] | (1u << (i % 8)));
    const std::size_t blocks = (bytes.size() + 14) / 16; // the 16-byte steps beyond the first byte
    bytes.resize(blocks * 16 + 1, kAlternatingBits);
    return bytes;
}
} // namespace retrochip::dpcm
