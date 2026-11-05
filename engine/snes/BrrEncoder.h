// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include <cstdint>
#include <vector>

namespace retrochip
{
namespace brr
{
// The range fullsnes recommends for decoded samples, which the encoder keeps to.
inline constexpr int kEncodeMin = -0x3FFA;
inline constexpr int kEncodeMax = 0x3FF8;
} // namespace brr

// The block a loop point falls on: the first block boundary at or after the sample, -1 for none.
constexpr int brrLoopBlockFor(int loopStartSample)
{
    return loopStartSample < 0 ? -1 : loopStartSample / 16 + (loopStartSample % 16 != 0 ? 1 : 0);
}

// Compresses mono samples to BRR, at the chip's rate (the caller resamples). pcm is -1 to +1,
// where 1 is the top of the recommended range, 0x3FF8; anything beyond is clipped, and NaN is 0.
//
// For each block the encoder tries every shift from 0 to 12 and every filter from 0 to 3, decodes
// each through brr::decodeSample from the decoder's own previous two samples, and keeps the
// smallest error, so what it measures is what is played. Decoded samples stay within
// brr::kEncodeMin to brr::kEncodeMax. The first block, and the loop block, use filter 0: the
// decoder jumps back to the loop block with whatever history it has.
//
// Returns whole 9-byte blocks, with the end flag on the last. loopStartSample is -1 for no loop;
// otherwise the loop flag goes on the last block too, and the loop starts at
// brrLoopBlockFor(loopStartSample), which the caller stores in SnesSample::loopBlock. A loop point
// at or past the end is no loop. Input that ends inside a block is padded with zeros, and no
// input gives no blocks.
//
// Takes time in proportion to the input and allocates, so never on the audio thread.
std::vector<std::uint8_t> encodeBrr(const std::vector<float>& pcm, int loopStartSample);
} // namespace retrochip
