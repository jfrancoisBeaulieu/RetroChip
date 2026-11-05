// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "Fti.h"

#include "Dpcm.h"

#include <algorithm>
#include <cctype>

// The layout is FamiTracker's InstrumentIO, as 0CC-FamiTracker writes it: "FTI" and a version such as "2.4", the
// chip, the name, five sequences, then what the chip adds. Numbers are little-endian.
namespace retrochip
{
namespace
{
constexpr int kOldestVersion = 21; // 2.1, the first with release points
constexpr int kNewestVersion = 25;
constexpr int kMaxNameLength = 1024;
constexpr int kMaxSequenceLength = 255;
constexpr int kMaxKeys = 128;
constexpr int kMaxSamples = 64;
constexpr int kMaxWaveSize = 256;
constexpr int kMaxWaveCount = 64;
constexpr int kMaxWaveSample = 15;
constexpr int kFirstMidiNote = 12; // FamiTracker's C-0

class Reader
{
public:
    Reader(const std::uint8_t* data, std::size_t size) : data(data), size(size) {}

    bool byte(int& value)
    {
        if (position >= size)
            return false;
        value = static_cast<std::int8_t>(data[position++]);
        return true;
    }

    bool int32(int& value)
    {
        if (size - position < 4 || position > size)
            return false;
        std::uint32_t raw = 0;
        for (int i = 0; i < 4; ++i)
            raw |= static_cast<std::uint32_t>(data[position++]) << (8 * i);
        value = static_cast<std::int32_t>(raw);
        return true;
    }

    // An int32 within [low, high].
    bool int32(int& value, int low, int high) { return int32(value) && value >= low && value <= high; }

    bool text(std::string& out, int maxLength)
    {
        int length = 0;
        if (!int32(length, 0, maxLength) || size - position < static_cast<std::size_t>(length))
            return false;
        out.assign(reinterpret_cast<const char*>(data + position), static_cast<std::size_t>(length));
        position += static_cast<std::size_t>(length);
        return true;
    }

private:
    const std::uint8_t* data;
    std::size_t size;
    std::size_t position = 0;
};

FtiParse failure(FtiError error)
{
    FtiParse result;
    result.error = error;
    return result;
}

bool readSequences(Reader& in, int version, FtiInstrument& instrument)
{
    int count = 0;
    if (!in.byte(count) || count < 0 || count > kFtiSequenceCount)
        return false;

    for (int i = 0; i < count; ++i)
    {
        int enabled = 0;
        if (!in.byte(enabled))
            return false;
        if (enabled == 0)
            continue;

        auto& sequence = instrument.sequences[static_cast<std::size_t>(i)];
        int length = 0;
        if (!in.int32(length, 0, kMaxSequenceLength) || !in.int32(sequence.loop, -1, std::max(length - 1, -1)))
            return false;
        if (version > 20 && !in.int32(sequence.release, -1, std::max(length - 1, -1)))
            return false;
        if (version >= 22 && !in.int32(sequence.setting, 0, 3))
            return false;

        sequence.values.resize(static_cast<std::size_t>(length));
        for (auto& value : sequence.values)
        {
            int v = 0;
            if (!in.byte(v))
                return false;
            value = static_cast<std::int8_t>(v);
        }
        sequence.enabled = length > 0;
    }
    return true;
}

bool readSamples(Reader& in, int version, FtiInstrument& instrument)
{
    struct RawKey
    {
        int note, slot, pitch, delta;
    };
    std::vector<RawKey> raw;

    int keyCount = 0;
    if (!in.int32(keyCount, 0, kMaxKeys))
        return false;
    for (int i = 0; i < keyCount; ++i)
    {
        RawKey key {};
        int sample = 0;
        if (!in.byte(key.note) || !in.byte(sample) || !in.byte(key.pitch) || key.note < 0)
            return false;
        key.pitch &= 0xFF;
        key.slot = (sample & 0xFF) - 1;
        key.delta = -1;
        if (version >= 24 && (!in.byte(key.delta) || key.delta < -1))
            return false;
        raw.push_back(key);
    }

    std::vector<int> slots;
    int sampleCount = 0;
    if (!in.int32(sampleCount, 0, kMaxSamples))
        return false;
    for (int i = 0; i < sampleCount; ++i)
    {
        int slot = 0;
        DpcmSample sample;
        std::string bytes;
        if (!in.int32(slot, 0, kMaxSamples - 1) || !in.text(sample.name, kMaxNameLength) || !in.text(bytes, dpcm::kMaxBytes) || bytes.empty())
            return false;
        sample.bytes.assign(bytes.begin(), bytes.end());
        slots.push_back(slot);
        instrument.samples.push_back(std::move(sample));
    }

    for (const auto& key : raw)
    {
        const auto found = std::find(slots.begin(), slots.end(), key.slot);
        const int note = kFirstMidiNote + key.note;
        if (found == slots.end() || note > 127)
            continue;
        FtiDpcmKey placed;
        placed.note = note;
        placed.sample = static_cast<int>(found - slots.begin());
        placed.rate = key.pitch & 0x0F;
        placed.loop = (key.pitch & 0x80) != 0;
        placed.delta = key.delta;
        instrument.keys.push_back(placed);
    }
    return true;
}

bool readWaves(Reader& in, int version, FtiInstrument& instrument)
{
    int waveSize = 0, position = 0, ignored = 0, waveCount = 0;
    if (!in.int32(waveSize, 4, kMaxWaveSize) || !in.int32(position, 0, kMaxWaveSize - 1))
        return false;
    if (version >= 25 && !in.int32(ignored))
        return false;
    if (!in.int32(waveCount, 1, kMaxWaveCount))
        return false;

    instrument.waves.assign(static_cast<std::size_t>(waveCount), std::vector<std::uint8_t>(static_cast<std::size_t>(waveSize)));
    for (auto& wave : instrument.waves)
        for (auto& sample : wave)
        {
            int v = 0;
            if (!in.byte(v) || v < 0 || v > kMaxWaveSample)
                return false;
            sample = static_cast<std::uint8_t>(v);
        }
    return true;
}
} // namespace

FtiParse parseFti(const std::uint8_t* data, std::size_t size)
{
    if (data == nullptr || size < 7 || data[0] != 'F' || data[1] != 'T' || data[2] != 'I')
        return failure(FtiError::notFti);
    if (!std::isdigit(data[3]) || data[4] != '.' || !std::isdigit(data[5]))
        return failure(FtiError::unsupportedVersion);
    const int version = (data[3] - '0') * 10 + (data[5] - '0');
    if (version < kOldestVersion || version > kNewestVersion)
        return failure(FtiError::unsupportedVersion);

    FtiInstrument instrument;
    const int chip = data[6];
    if (chip != static_cast<int>(FtiChip::Ricoh2A03) && chip != static_cast<int>(FtiChip::Vrc6) && chip != static_cast<int>(FtiChip::N163))
        return failure(FtiError::unsupportedChip);
    instrument.chip = static_cast<FtiChip>(chip);

    Reader in(data + 7, size - 7);
    if (!in.text(instrument.name, kMaxNameLength) || !readSequences(in, version, instrument))
        return failure(FtiError::damaged);
    if (instrument.chip == FtiChip::Ricoh2A03 && !readSamples(in, version, instrument))
        return failure(FtiError::damaged);
    if (instrument.chip == FtiChip::N163 && !readWaves(in, version, instrument))
        return failure(FtiError::damaged);

    FtiParse result;
    result.instrument = std::move(instrument);
    return result;
}
} // namespace retrochip
