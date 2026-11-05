// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "Tfi.h"

#include <array>

namespace retrochip
{
namespace
{
constexpr std::size_t kHeaderSize = 2;
constexpr std::size_t kOperatorSize = 10;

// The blocks follow the chip's registers, at offsets 0, 4, 8, 12, so they hold operators 1, 3, 2, 4. The format
// has no specification that says so: Furnace, OPN2 Bank Editor and tfi2eif all read it this way.
constexpr std::array<std::size_t, 4> kOperatorOfBlock { 0, 2, 1, 3 };

// The file has 0 to 6 with 3 for no detune, and the register has 1 to 3 sharp and 5 to 7 flat.
constexpr std::array<int, 7> kDetuneRegister { 7, 6, 5, 0, 1, 2, 3 };

constexpr std::array<int, kOperatorSize> kOperatorMaximum { 15, 6, 127, 3, 31, 31, 31, 15, 15, 15 };
constexpr int kMaxAlgorithmOrFeedback = 7;
constexpr int kSsgEnableBit = 8; // below it, the chip's SSG-EG is off and the other bits mean nothing
} // namespace

std::optional<FmPatch> parseTfi(const std::uint8_t* data, std::size_t size)
{
    if (data == nullptr || size != kTfiSize || data[0] > kMaxAlgorithmOrFeedback || data[1] > kMaxAlgorithmOrFeedback)
        return std::nullopt;

    FmPatch patch {};
    patch.name = "";
    patch.algorithm = data[0];
    patch.feedback = data[1];

    for (std::size_t block = 0; block < kOperatorOfBlock.size(); ++block)
    {
        const auto* bytes = data + kHeaderSize + block * kOperatorSize;
        for (std::size_t field = 0; field < kOperatorSize; ++field)
            if (bytes[field] > kOperatorMaximum[field])
                return std::nullopt;

        auto& op = patch.ops[kOperatorOfBlock[block]];
        op.mul = bytes[0];
        op.dt = kDetuneRegister[bytes[1]];
        op.tl = bytes[2];
        op.rs = bytes[3];
        op.ar = bytes[4];
        op.dr = bytes[5];
        op.sr = bytes[6];
        op.rr = bytes[7];
        op.sl = bytes[8];
        op.ssg = bytes[9] >= kSsgEnableBit ? bytes[9] : 0;
    }
    return patch;
}
} // namespace retrochip
