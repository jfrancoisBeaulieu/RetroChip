// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace retrochip
{
// A sample in the chip's format, immutable once published to the engine.
struct SnesSample
{
    std::vector<std::uint8_t> brr; // 9-byte blocks; the header's end flag marks the last one
    int loopBlock = -1;            // block to continue from after the last one, -1 for none
    int rootNote = 60;             // the note that plays the sample at the chip's 32 kHz
    std::string name;
};

// Formulas from fullsnes, "SNES APU DSP BRR Samples" and "SNES APU DSP BRR Pitch".
namespace brr
{
inline constexpr int kBytesPerBlock = 9;
inline constexpr int kSamplesPerBlock = 16;

// Bit 0 of a block's header. Bit 1, the loop flag, says nothing here: loopBlock decides.
constexpr bool hasEndFlag(std::uint8_t header) { return (header & 1) != 0; }

// One decoded sample: the signed nibble (-8 to 7) at a block's shift and filter, on top of the two
// previous samples. decodeBlock is this, 16 times; the encoder uses it to try nibbles.
int decodeSample(int nibble, int shift, int filter, int old, int older);

// Decodes a block to 15-bit samples. old and older are the two previous samples, and hold the
// block's last two on return.
void decodeBlock(const std::uint8_t* block, int& old, int& older, std::int16_t out[16]);

// 4-point Gaussian interpolation of window[0] (oldest) to window[3] (newest), with index 0 to 255.
// Deliberate difference from the chip: every addition saturates, including the second one, whose
// overflow would click.
int interpolate(const std::int16_t* window, int index);
} // namespace brr
} // namespace retrochip
