// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include <array>
#include <cstdint>
#include <vector>

// The 2A03's delta-modulation channel, from the NESdev wiki's APU DMC page.
namespace retrochip::dpcm
{
// CPU cycles per bit, NTSC, for rates 0 to 15: about 4182 Hz to 33144 Hz.
inline constexpr std::array<int, 16> kRatePeriods = { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 };
inline constexpr int kFastestRate = 15;
inline constexpr int kMaxBytes = 255 * 16 + 1; // the length register counts 16 bytes, plus one
inline constexpr int kMiddleLevel = 64;
inline constexpr int kMaxLevel = 127;

double rateHz(int rate);

// The rate whose speed is closest to `hz`, in pitch.
int nearestRate(double hz);

// The level after one bit, as the chip moves it: up or down by 2, never past 0 or 127.
constexpr int nextLevel(int level, bool up)
{
    if (up)
        return level <= kMaxLevel - 2 ? level + 2 : level;
    return level >= 2 ? level - 2 : level;
}

// Turns samples at the fastest rate (-1 to 1) into bits, starting from the middle, each bit going toward the next
// sample. The length is padded to what the chip can play, 16 bytes and one, with bits that go up and down in turn.
// At most kMaxBytes; anything after is left out.
std::vector<std::uint8_t> encode(const std::vector<float>& samples);
} // namespace retrochip::dpcm
