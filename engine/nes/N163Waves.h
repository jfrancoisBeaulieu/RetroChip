// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace retrochip
{
// The Namco 163's waves that come with RetroChip, drawn for this project: 32 steps of 4 bits each.
inline constexpr int kN163WaveCount = 8;
inline constexpr int kN163WaveLength = 32;
inline constexpr const char* kN163WaveNames[kN163WaveCount] = { "Sine", "Triangle", "Saw", "Square", "Pulse", "Organ", "Hollow", "Bell" };

// Built on first use, which the engine makes on its own thread before any audio.
const std::array<std::vector<std::uint8_t>, kN163WaveCount>& n163Waves();
} // namespace retrochip
