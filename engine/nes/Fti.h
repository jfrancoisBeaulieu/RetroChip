// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace retrochip
{
// The chips a FamiTracker instrument (.fti) can be for, with the file's own numbers. RetroChip plays the 2A03's,
// the VRC6's and the Namco 163's.
enum class FtiChip
{
    Ricoh2A03 = 1,
    Vrc6 = 2,
    Vrc7 = 3,
    Fds = 4,
    N163 = 5,
    S5B = 6
};

// The order of the file's five sequences. The last is the pulse duty or noise mode, or the wave on the N163.
enum FtiSequenceType
{
    kFtiVolume,
    kFtiArpeggio,
    kFtiPitch,
    kFtiHiPitch,
    kFtiDuty,
    kFtiSequenceCount
};

// Settings of the arpeggio and pitch sequences.
inline constexpr int kArpeggioAbsolute = 0, kArpeggioFixed = 1, kArpeggioRelative = 2, kArpeggioScheme = 3;
inline constexpr int kPitchRelative = 0, kPitchAbsolute = 1;
inline constexpr int kVolume64Steps = 1; // the VRC6 sawtooth's

// One value per 60 Hz step. loop and release are positions in values, or -1.
struct FtiSequence
{
    bool enabled = false;
    std::vector<std::int8_t> values;
    int loop = -1;
    int release = -1;
    int setting = 0;
};

// The 2A03's delta-modulation samples: bits read from bit 0 up, each moving the level by 2.
struct DpcmSample
{
    std::vector<std::uint8_t> bytes;
    std::string name;
};

// A key of a 2A03 instrument that plays one of its samples.
struct FtiDpcmKey
{
    int note = 0;   // MIDI
    int sample = 0; // into FtiInstrument::samples
    int rate = 15;  // 0 to 15, the chip's rate table
    bool loop = false;
    int delta = -1; // the level the sample starts from, 0 to 127, or -1 to start from the middle
};

struct FtiInstrument
{
    FtiChip chip = FtiChip::Ricoh2A03;
    std::string name;
    std::array<FtiSequence, kFtiSequenceCount> sequences;

    std::vector<FtiDpcmKey> keys;     // 2A03 only
    std::vector<DpcmSample> samples;

    std::vector<std::vector<std::uint8_t>> waves; // N163 only: 4-bit samples, all of one length
};

enum class FtiError
{
    none,
    notFti,          // no "FTI" header
    unsupportedVersion,
    unsupportedChip, // VRC7, FDS, Sunsoft 5B
    damaged          // cut short, or a value out of its range
};

struct FtiParse
{
    std::optional<FtiInstrument> instrument;
    FtiError error = FtiError::none;
};

// Reads files from FamiTracker 0.4 and its successors, versions 2.1 to 2.5. FamiTracker numbers notes from C-0,
// which is MIDI note 12. Keys outside MIDI's range, and keys whose sample is missing, are left out.
FtiParse parseFti(const std::uint8_t* data, std::size_t size);
} // namespace retrochip
