// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

// Hardware values from the NESdev wiki (APU, APU Pulse, Triangle, Noise, Mixer pages).
namespace retrochip::nes
{
inline constexpr double kCpuClock = 1789773.0;
inline constexpr double kTicksPerSecond = 60.0;
inline constexpr int kMinAudiblePulseTimer = 8;
inline constexpr int kMinTriangleTimer = 2;

inline constexpr std::uint8_t kDutySequences[4][8] = {
    { 0, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 1, 1, 0, 0, 0, 0, 0 },
    { 0, 1, 1, 1, 1, 0, 0, 0 },
    { 1, 0, 0, 1, 1, 1, 1, 1 },
};

inline constexpr std::uint8_t kTriangleSequence[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};
inline constexpr int kTriangleFirstZeroStep = 15;

inline constexpr std::array<int, 16> kNoisePeriods = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};

constexpr int noiseIndexForNote(int note)
{
    return std::clamp(7 - (note - 60), 0, 15);
}

constexpr std::uint16_t stepLfsr(std::uint16_t state, bool shortMode)
{
    const int feedback = (state & 1) ^ ((state >> (shortMode ? 6 : 1)) & 1);
    return static_cast<std::uint16_t>((state >> 1) | (feedback << 14));
}

// Each voice goes through the console's mixer curve on its own, then voices are summed.
inline constexpr float kMixScale = 2.0f;

constexpr std::array<float, 16> makeAmpTable(double numerator, double denominator, int weight)
{
    std::array<float, 16> table {};
    for (int level = 1; level < 16; ++level)
        table[static_cast<size_t>(level)] =
            kMixScale * static_cast<float>(numerator / (denominator / (level * weight) + 100.0));
    return table;
}

inline constexpr auto kPulseAmp = makeAmpTable(95.52, 8128.0, 1);
inline constexpr auto kTriangleAmp = makeAmpTable(163.67, 24329.0, 3);
inline constexpr auto kNoiseAmp = makeAmpTable(163.67, 24329.0, 2);

// The delta-modulation channel's 128 levels, through the same curve as the triangle and the noise.
constexpr std::array<float, 128> makeDmcTable()
{
    std::array<float, 128> table {};
    for (int level = 1; level < 128; ++level)
        table[static_cast<size_t>(level)] = kMixScale * static_cast<float>(163.67 / (24329.0 / level + 100.0));
    return table;
}
inline constexpr auto kDmcAmp = makeDmcTable();

// The cartridge chips are linear. The VRC6's pulse at its full volume matches the 2A03's, as the NESdev wiki's VRC6
// page says; the N163's full-range wave at full volume swings as far as a 2A03 pulse.
inline constexpr float kVrc6Unit = kPulseAmp[15] / 15.0f;
inline constexpr float kN163Unit = kPulseAmp[15] / 225.0f;
inline constexpr int kVrc6MaxTimer = 4095;
inline constexpr int kVrc6DutySteps = 16;
inline constexpr int kSawSteps = 7;
inline constexpr double kSawMaxRate = 42.0; // above it the accumulator would wrap
inline constexpr int kN163CyclesPerChannel = 15;
inline constexpr int kN163MaxChannels = 8;
} // namespace retrochip::nes
