// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Builds a small valid SoundFont 2 file in memory, so no test needs an external file.
//
// Two looping sine samples, in two key zones of one instrument: notes below `splitKey` play the
// low sample (root A3, 220 Hz, period 100 frames) and notes from `splitKey` play the high one
// (root A4, 440 Hz, period 50 frames). Both are stored at 22 kHz, with the 46 zero frames the
// format puts after each sample. Each preset plays that one instrument.
namespace testsf2
{
struct Preset
{
    std::string name;
    int bank = 0;
    int program = 0;
};

struct TestSf2Options
{
    double lowAmplitude = 0.25;  // of full scale, for notes below the split
    double highAmplitude = 0.8;  // of full scale, for notes from the split
    double releaseSeconds = 0.2; // volume envelope release
    bool loop = true;            // continuous loop over the whole sample
    int splitKey = 60;
    int filterCents = 13500;     // initial filter cutoff in absolute cents; 13500 is open
    std::vector<Preset> presets { { "Test Sine", 0, 0 } };
};

constexpr int kStoredRate = 22000;
constexpr int kLowPeriod = 100;
constexpr int kHighPeriod = 50;
constexpr int kLowFrames = 40 * kLowPeriod;  // 40 periods, 0.18 s
constexpr int kHighFrames = 80 * kHighPeriod; // 80 periods, 0.18 s
constexpr int kPadFrames = 46;
constexpr int kLowRootKey = 57;
constexpr int kHighRootKey = 69;

class Writer
{
public:
    std::vector<std::uint8_t> bytes;

    void u8(int v) { bytes.push_back(static_cast<std::uint8_t>(v)); }
    void u16(int v)
    {
        u8(v & 0xFF);
        u8((v >> 8) & 0xFF);
    }
    void u32(std::uint32_t v)
    {
        u16(static_cast<int>(v & 0xFFFF));
        u16(static_cast<int>(v >> 16));
    }
    void id(const char* four)
    {
        for (int i = 0; i < 4; ++i)
            u8(four[i]);
    }
    void name(const std::string& s, size_t width)
    {
        for (size_t i = 0; i < width; ++i)
            u8(i < s.size() && i + 1 < width ? s[i] : 0);
    }
    void append(const Writer& other) { bytes.insert(bytes.end(), other.bytes.begin(), other.bytes.end()); }
};

inline Writer chunk(const char* fourcc, const Writer& body)
{
    Writer w;
    w.id(fourcc);
    w.u32(static_cast<std::uint32_t>(body.bytes.size()));
    w.append(body);
    return w;
}

inline Writer list(const char* type, const Writer& body)
{
    Writer inner;
    inner.id(type);
    inner.append(body);
    return chunk("LIST", inner);
}

inline int timecents(double seconds) { return static_cast<int>(std::lround(1200.0 * std::log2(seconds))); }

// One generator: its operator number and raw 16-bit amount. A key or velocity range packs the low
// end in the low byte and the high end in the high byte (see range()).
struct Gen
{
    int op;
    int amount;
};

constexpr int kKeyRange = 43, kVelRange = 44, kInstrument = 41, kSampleId = 53, kSampleModes = 54;
constexpr int kInitialFilterFc = 8, kPan = 17, kReleaseVolEnv = 38, kAttenuation = 48;

inline int range(int lo, int hi) { return lo | (hi << 8); }

// A zone is a list of generators in the order they are written. The sample ID (or instrument) last.
using Zone = std::vector<Gen>;

struct InstrumentSpec
{
    std::string name;
    std::vector<Zone> zones;
};

struct PresetSpec
{
    std::string name;
    int bank = 0;
    int program = 0;
    std::vector<Zone> zones;
};

// Writes a whole file from its tables: the two test samples, any presets and instruments.
inline std::vector<std::uint8_t> buildSf2(const std::vector<PresetSpec>& presets, const std::vector<InstrumentSpec>& instruments,
                                          const TestSf2Options& o = {})
{
    // INFO
    Writer info;
    {
        Writer ifil;
        ifil.u16(2);
        ifil.u16(1);
        info.append(chunk("ifil", ifil));
        Writer isng;
        isng.name("EMU8000", 8);
        info.append(chunk("isng", isng));
        Writer inam;
        inam.name("Test", 8);
        info.append(chunk("INAM", inam));
    }

    // Sample data: low sample, 46 zeros, high sample, 46 zeros.
    Writer smpl;
    const auto sine = [&smpl](int frames, int period, double amplitude)
    {
        for (int i = 0; i < frames; ++i)
            smpl.u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(
                std::lround(amplitude * 32767.0 * std::sin(2.0 * 3.14159265358979323846 * i / period)))));
        for (int i = 0; i < kPadFrames; ++i)
            smpl.u16(0);
    };
    sine(kLowFrames, kLowPeriod, o.lowAmplitude);
    sine(kHighFrames, kHighPeriod, o.highAmplitude);
    const std::uint32_t highStart = kLowFrames + kPadFrames;

    Writer sdta;
    sdta.append(chunk("smpl", smpl));

    Writer phdr, pbag, pgen;
    int pbagCount = 0, pgenCount = 0;
    for (const auto& preset : presets)
    {
        phdr.name(preset.name, 20);
        phdr.u16(preset.program);
        phdr.u16(preset.bank);
        phdr.u16(pbagCount);
        phdr.u32(0);
        phdr.u32(0);
        phdr.u32(0);
        for (const auto& zone : preset.zones)
        {
            pbag.u16(pgenCount);
            pbag.u16(0);
            ++pbagCount;
            for (const auto& gen : zone)
            {
                pgen.u16(gen.op);
                pgen.u16(gen.amount);
                ++pgenCount;
            }
        }
    }
    phdr.name("EOP", 20);
    phdr.u16(0);
    phdr.u16(0);
    phdr.u16(pbagCount);
    phdr.u32(0);
    phdr.u32(0);
    phdr.u32(0);
    pbag.u16(pgenCount);
    pbag.u16(0);
    pgen.u16(0);
    pgen.u16(0);

    Writer pmod;
    for (int i = 0; i < 5; ++i)
        pmod.u16(0);

    Writer inst, ibag, igen;
    int ibagCount = 0, igenCount = 0;
    for (const auto& instrument : instruments)
    {
        inst.name(instrument.name, 20);
        inst.u16(ibagCount);
        for (const auto& zone : instrument.zones)
        {
            ibag.u16(igenCount);
            ibag.u16(0);
            ++ibagCount;
            for (const auto& gen : zone)
            {
                igen.u16(gen.op);
                igen.u16(gen.amount);
                ++igenCount;
            }
        }
    }
    inst.name("EOI", 20);
    inst.u16(ibagCount);
    ibag.u16(igenCount);
    ibag.u16(0);
    igen.u16(0);
    igen.u16(0);

    Writer imod;
    for (int i = 0; i < 5; ++i)
        imod.u16(0);

    Writer shdr;
    const auto header = [&shdr](const char* name, std::uint32_t start, int frames, int rootKey)
    {
        shdr.name(name, 20);
        shdr.u32(start);
        shdr.u32(start + static_cast<std::uint32_t>(frames));
        shdr.u32(start); // loop start
        shdr.u32(start + static_cast<std::uint32_t>(frames)); // loop end, exclusive
        shdr.u32(kStoredRate);
        shdr.u8(rootKey);
        shdr.u8(0);
        shdr.u16(0);
        shdr.u16(1); // mono sample
    };
    header("Low", 0, kLowFrames, kLowRootKey);
    header("High", highStart, kHighFrames, kHighRootKey);
    shdr.name("EOS", 20);
    for (int i = 0; i < 5; ++i)
        shdr.u32(0);
    shdr.u8(0);
    shdr.u8(0);
    shdr.u16(0);
    shdr.u16(0);

    Writer pdta;
    pdta.append(chunk("phdr", phdr));
    pdta.append(chunk("pbag", pbag));
    pdta.append(chunk("pmod", pmod));
    pdta.append(chunk("pgen", pgen));
    pdta.append(chunk("inst", inst));
    pdta.append(chunk("ibag", ibag));
    pdta.append(chunk("imod", imod));
    pdta.append(chunk("igen", igen));
    pdta.append(chunk("shdr", shdr));

    Writer body;
    body.id("sfbk");
    body.append(list("INFO", info));
    body.append(list("sdta", sdta));
    body.append(list("pdta", pdta));
    return chunk("RIFF", body).bytes;
}

// The usual layout: every preset plays the one instrument, whose two zones carry the options.
inline std::vector<std::uint8_t> buildTestSf2(const TestSf2Options& o = {})
{
    const auto zone = [&o](int loKey, int hiKey, int sampleId)
    {
        return Zone { { kKeyRange, range(loKey, hiKey) },
                      { kInitialFilterFc, o.filterCents },
                      { kReleaseVolEnv, timecents(o.releaseSeconds) },
                      { kSampleModes, o.loop ? 1 : 0 },
                      { kSampleId, sampleId } };
    };
    std::vector<PresetSpec> presets;
    for (const auto& preset : o.presets)
        presets.push_back({ preset.name, preset.bank, preset.program, { { { kInstrument, 0 } } } });
    return buildSf2(presets, { { "Test Sine", { zone(0, o.splitKey - 1, 0), zone(o.splitKey, 127, 1) } } }, o);
}

// Offsets of every chunk header (the 4-character id) in the file, walking RIFF and LIST nesting.
inline void collectHeaders(const std::vector<std::uint8_t>& f, size_t pos, size_t end, std::vector<size_t>& out)
{
    while (pos + 8 <= end)
    {
        out.push_back(pos);
        const size_t size = f[pos + 4] | (f[pos + 5] << 8) | (f[pos + 6] << 16) | (static_cast<size_t>(f[pos + 7]) << 24);
        const bool container = (f[pos] == 'L' && f[pos + 1] == 'I') || f[pos] == 'R';
        if (container && pos + 12 <= end)
            collectHeaders(f, pos + 12, std::min(end, pos + 8 + size), out);
        pos += 8 + size;
    }
}

inline std::vector<size_t> chunkHeaderOffsets(const std::vector<std::uint8_t>& f)
{
    std::vector<size_t> out;
    collectHeaders(f, 0, f.size(), out);
    return out;
}
} // namespace testsf2
