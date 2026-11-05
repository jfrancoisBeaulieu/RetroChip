// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include "FmPatches.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace retrochip
{
// A TFI file (TFM Music Maker) is one FM timbre and no sound data: the algorithm, the feedback, then 10
// bytes per operator, namely mul, dt, tl, rs, ar, dr, sr, rr, sl, ssg-eg.
inline constexpr std::size_t kTfiSize = 42;

// The patch in a file's bytes, in this project's units and operator order, or nothing if the size is not
// kTfiSize or a value is out of its range. The patch has an empty name, since the file holds none.
std::optional<FmPatch> parseTfi(const std::uint8_t* data, std::size_t size);
} // namespace retrochip
