// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "N163Waves.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace retrochip
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kTop = 15.0;

double sine(double t, double harmonic) { return std::sin(2.0 * kPi * harmonic * t); }

// One period, scaled so that it fills the chip's 16 levels.
std::vector<std::uint8_t> draw(const std::function<double(double)>& shape)
{
    std::vector<double> values(kN163WaveLength);
    for (int i = 0; i < kN163WaveLength; ++i)
        values[static_cast<std::size_t>(i)] = shape(static_cast<double>(i) / kN163WaveLength);
    const auto [low, high] = std::minmax_element(values.begin(), values.end());
    const double lowest = *low, range = std::max(*high - *low, 1e-9);

    std::vector<std::uint8_t> wave(kN163WaveLength);
    for (std::size_t i = 0; i < wave.size(); ++i)
        wave[i] = static_cast<std::uint8_t>(std::lround((values[i] - lowest) / range * kTop));
    return wave;
}
} // namespace

const std::array<std::vector<std::uint8_t>, kN163WaveCount>& n163Waves()
{
    static const std::array<std::vector<std::uint8_t>, kN163WaveCount> waves = {
        draw([](double t) { return sine(t, 1); }),
        draw([](double t) { return t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t; }),
        draw([](double t) { return 2.0 * t - 1.0; }),
        draw([](double t) { return t < 0.5 ? 1.0 : -1.0; }),
        draw([](double t) { return t < 0.25 ? 1.0 : -1.0; }),
        draw([](double t) { return sine(t, 1) + 0.5 * sine(t, 2) + 0.25 * sine(t, 3) + 0.125 * sine(t, 4); }),
        draw([](double t) { return sine(t, 1) + sine(t, 3) / 3.0 + sine(t, 5) / 5.0; }),
        draw([](double t) { return sine(t, 1) + 0.6 * sine(t, 4) + 0.35 * sine(t, 7); }),
    };
    return waves;
}
} // namespace retrochip
