// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "Brr.h"

#include "GaussTable.h"

#include <algorithm>

namespace retrochip::brr
{
namespace
{
constexpr int kMaxNormalShift = 12;

constexpr int saturate16(int value) { return std::clamp(value, -0x8000, 0x7FFF); }

// Keeps the low 15 bits, with bit 14 as the sign: +4000h..+7FFFh becomes -4000h..-1, and
// -8000h..-4001h becomes 0..+3FFFh.
constexpr int wrap15(int value) { return ((value & 0x7FFF) ^ 0x4000) - 0x4000; }

constexpr int signedNibble(int nibble) { return ((nibble & 0x0F) ^ 0x08) - 0x08; }

int predict(int filter, int old, int older)
{
    switch (filter)
    {
        case 1: return old + ((-old) >> 4);
        case 2: return 2 * old + ((-3 * old) >> 5) - older + (older >> 4);
        case 3: return 2 * old + ((-13 * old) >> 6) - older + ((3 * older) >> 4);
        default: return 0;
    }
}
} // namespace

int decodeSample(int nibble, int shift, int filter, int old, int older)
{
    // Shifts 13 to 15 are reserved, and act as shift 12 with only the nibble's sign left.
    const int sample = shift <= kMaxNormalShift ? (nibble << shift) >> 1 : ((nibble >> 3) << kMaxNormalShift) >> 1;
    return wrap15(saturate16(sample + predict(filter, old, older)));
}

void decodeBlock(const std::uint8_t* block, int& old, int& older, std::int16_t out[16])
{
    const int header = block[0];
    const int shift = header >> 4;
    const int filter = (header >> 2) & 3;

    for (int i = 0; i < kSamplesPerBlock; ++i)
    {
        const int byte = block[1 + i / 2];
        const int nibble = signedNibble(i % 2 == 0 ? byte >> 4 : byte);
        const int decoded = decodeSample(nibble, shift, filter, old, older);
        out[i] = static_cast<std::int16_t>(decoded);
        older = old;
        old = decoded;
    }
}

int interpolate(const std::int16_t* window, int index)
{
    const auto i = static_cast<size_t>(index);
    const auto& g = snes::kGaussTable;
    int out = (g[0x0FF - i] * window[0]) >> 10;
    out = saturate16(out + ((g[0x1FF - i] * window[1]) >> 10));
    out = saturate16(out + ((g[0x100 + i] * window[2]) >> 10));
    out = saturate16(out + ((g[i] * window[3]) >> 10));
    return out >> 1;
}
} // namespace retrochip::brr
