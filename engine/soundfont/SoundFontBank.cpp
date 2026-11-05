// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "SoundFontBank.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace retrochip::soundfont
{
namespace
{
std::atomic<long long> allocations { 0 };
}

long long allocationCount() { return allocations.load(std::memory_order_relaxed); }

// TinySoundFont's allocator. It counts, and it zeroes: TinySoundFont leaves some of the memory it
// asks for unwritten when a file is inconsistent, and zeros there are inert where garbage is not.
void* countedMalloc(std::size_t size)
{
    allocations.fetch_add(1, std::memory_order_relaxed);
    return std::calloc(1, size > 0 ? size : 1);
}

void* countedRealloc(void* p, std::size_t size)
{
    allocations.fetch_add(1, std::memory_order_relaxed);
    return std::realloc(p, size);
}

void countedFree(void* p) { std::free(p); }
} // namespace retrochip::soundfont

#define TSF_MALLOC(size) retrochip::soundfont::countedMalloc(size)
#define TSF_REALLOC(p, size) retrochip::soundfont::countedRealloc(p, size)
#define TSF_FREE(p) retrochip::soundfont::countedFree(p)
#define TSF_NO_STDIO
#define TSF_IMPLEMENTATION
// Third-party code: found through a system include path, so its warnings stay off.
#include <tsf.h>

namespace retrochip
{
namespace
{
// ---- Checking a file before TinySoundFont reads it ------------------------------------------------
//
// tsf_load trusts the indexes and sizes in the file. A damaged one makes it read or write outside
// its buffers. So the file is checked here first, in three steps that each follow what tsf_load does:
// the chunks it would read (a replay of its reading, below), the indexes in the tables it reads, and
// the number of regions it would write for each preset against the number it allocated.

std::uint32_t le32(const std::uint8_t* p)
{
    return p[0] | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

int le16(const std::uint8_t* p) { return p[0] | (p[1] << 8); }

bool isId(const char* a, const char* id) { return std::memcmp(a, id, 4) == 0; }

struct Table
{
    const char* id;
    std::size_t recordSize;
    std::size_t at = 0;
    std::size_t count = 0;
    bool seen = false;
};

enum { Phdr, Pbag, Pmod, Pgen, Inst, Ibag, Imod, Igen, Shdr, TableCount };

struct Layout
{
    Table tables[TableCount] = { { "phdr", 38 }, { "pbag", 4 }, { "pmod", 10 }, { "pgen", 4 }, { "inst", 22 },
                                 { "ibag", 4 }, { "imod", 10 }, { "igen", 4 }, { "shdr", 46 } };
    std::size_t sampleBytes = 0;
    bool hasSamples = false;
};

// Replays tsf_load's reading of the chunks over the same bytes: the same position, the same size
// bookkeeping (kept in 32 bits, as tsf_load keeps it), the same reading of what follows after it
// stops early. Where it reads, the replay records the tables and the samples. What tsf_load accepts
// that does not show in the replay is refused rather than guessed at: a header cut short by the end
// of the file, a table or the samples reaching past the end, a table twice, or too many steps.
class Replay
{
public:
    Replay(const std::uint8_t* bytes, std::size_t size) : d(bytes), n(static_cast<std::uint32_t>(size)) {}

    bool run(Layout& layout)
    {
        // The RIFF chunk: tsf_load reads it without a parent, so its size is not held to the file.
        Parent riff;
        Chunk head;
        if (!read(nullptr, head) || !head.is("sfbk") || failed)
            return false;
        riff.size = head.size;

        Chunk list;
        while (read(&riff, list))
        {
            if (list.is("pdta"))
            {
                Chunk chunk;
                Parent parent { list.size };
                while (read(&parent, chunk))
                {
                    bool matched = false;
                    for (auto& table : layout.tables)
                        if (chunk.is(table.id) && chunk.size % table.recordSize == 0)
                        {
                            if (table.seen || chunk.size > n - pos)
                                return false;
                            table.seen = true;
                            table.at = pos;
                            table.count = chunk.size / table.recordSize;
                            pos += chunk.size;
                            matched = true;
                            break;
                        }
                    if (!matched)
                        skip(chunk.size);
                }
            }
            else if (list.is("sdta"))
            {
                Chunk chunk;
                Parent parent { list.size };
                while (read(&parent, chunk))
                {
                    if (chunk.is("smpl") && !layout.hasSamples && chunk.size >= 2)
                    {
                        if (chunk.size > n - pos)
                            return false;
                        layout.hasSamples = true;
                        layout.sampleBytes = chunk.size;
                        pos += chunk.size;
                    }
                    else
                        skip(chunk.size);
                }
            }
            else
                skip(list.size);

            if (failed)
                return false;
        }
        if (failed)
            return false;

        for (const auto& table : layout.tables)
            if (!table.seen)
                return false;
        return layout.hasSamples;
    }

private:
    struct Parent
    {
        std::uint32_t size = 0;
    };

    struct Chunk
    {
        char id[4] = { 0, 0, 0, 0 };
        std::uint32_t size = 0;
        bool is(const char* name) const { return isId(id, name); }
    };

    // tsf_riffchunk_read.
    bool read(Parent* parent, Chunk& chunk)
    {
        if (++steps > 100000)
            return fail();
        if (parent != nullptr && 8 > parent->size)
            return false;
        if (pos == n)
            return false; // nothing left to read
        if (n - pos < 8)
            return fail(); // a header cut short by the end of the file
        std::memcpy(chunk.id, d + pos, 4);
        if (!plausible(chunk.id))
        {
            pos += 4;
            return false;
        }
        chunk.size = le32(d + pos + 4);
        pos += 8;
        if (parent != nullptr && 8ull + chunk.size > parent->size)
            return false;
        if (parent != nullptr)
            parent->size -= 8 + chunk.size;

        const bool isRiff = isId(chunk.id, "RIFF"), isList = isId(chunk.id, "LIST");
        if (isRiff && parent != nullptr)
            return false;
        if (!isRiff && !isList)
            return true;

        // Its type replaces its id, and takes four bytes off its size.
        if (pos == n)
            return false;
        if (n - pos < 4)
            return fail();
        std::memcpy(chunk.id, d + pos, 4);
        if (!plausible(chunk.id))
        {
            pos += 4;
            return false;
        }
        pos += 4;
        chunk.size -= 4;
        return true;
    }

    // The stream's skip, which refuses to go past the end and, because its sum is 32-bit, can wrap.
    void skip(std::uint32_t count)
    {
        const std::uint32_t target = pos + count;
        if (target <= n)
            pos = target;
    }

    static bool plausible(const char* id) { return id[0] > ' ' && id[0] < 'z'; }

    bool fail()
    {
        failed = true;
        return false;
    }

    const std::uint8_t* d;
    std::uint32_t n;
    std::uint32_t pos = 0;
    int steps = 0;
    bool failed = false;
};

// Every index must land inside the table it names, as tsf_load_presets needs: ranges that only grow,
// ending inside the next table, and room for the terminal record it reads past each range.
bool checkIndexes(const std::uint8_t* d, const Layout& layout)
{
    const auto& phdr = layout.tables[Phdr];
    const auto& pbag = layout.tables[Pbag];
    const auto& pgen = layout.tables[Pgen];
    const auto& inst = layout.tables[Inst];
    const auto& ibag = layout.tables[Ibag];
    const auto& igen = layout.tables[Igen];
    const auto& shdr = layout.tables[Shdr];

    if (phdr.count < 2 || pbag.count < 1 || pgen.count < 1 || inst.count < 1 || ibag.count < 1 || igen.count < 1 || shdr.count < 1)
        return false;

    const auto growing = [d](const Table& table, std::size_t fieldAt, std::size_t limit)
    {
        int previous = 0;
        for (std::size_t i = 0; i < table.count; ++i)
        {
            const int value = le16(d + table.at + i * table.recordSize + fieldAt);
            if (value < previous || static_cast<std::size_t>(value) > limit)
                return false;
            previous = value;
        }
        return true;
    };

    if (!growing(phdr, 24, pbag.count - 1) || !growing(pbag, 0, pgen.count) || !growing(inst, 20, ibag.count - 1)
        || !growing(ibag, 0, igen.count))
        return false;

    // An instrument reference must leave room for the terminal record after it.
    for (std::size_t i = 0; i < pgen.count; ++i)
    {
        const std::uint8_t* gen = d + pgen.at + i * 4;
        if (le16(gen) == 41 && static_cast<std::size_t>(le16(gen + 2)) == inst.count - 1)
            return false;
    }
    for (std::size_t i = 0; i < igen.count; ++i)
    {
        const std::uint8_t* gen = d + igen.at + i * 4;
        if (le16(gen) == 53 && static_cast<std::size_t>(le16(gen + 2)) >= shdr.count)
            return false;
    }
    return true;
}

// tsf_load_presets allocates each preset's regions after one count, then fills them in a second pass
// that decides differently which zones make a region: the count judges each zone by its own key and
// velocity ranges, the fill by the ranges it inherits from global zones, and promotes a zone without a
// sample to global. A file where the fill writes more than the count allocated overflows the heap, so
// both passes are played out here on the ranges alone, and the file is refused when the fill would
// write more. A fill that writes fewer is left alone: the regions it does not reach stay zeros, which
// the allocator guarantees and sanitizeRegions switches off.
struct Ranges
{
    std::uint8_t lokey = 0, hikey = 127, lovel = 0, hivel = 127;

    // The two generators that TinySoundFont's region_operator reads ranges from.
    void apply(const std::uint8_t* gen)
    {
        if (le16(gen) == 43)
            lokey = gen[2], hikey = gen[3];
        else if (le16(gen) == 44)
            lovel = gen[2], hivel = gen[3];
    }
};

// The most regions one file may ask TinySoundFont for, over all its presets. A real font has tens
// of thousands; a few hundred kilobytes of references can ask for billions.
constexpr std::uint64_t kMaxRegions = 2000000;
// The most generators and zones the two counting passes may step through. Zones without a sample
// ask for no region, and cost time all the same. A real font takes a few million.
constexpr std::uint64_t kMaxSteps = 64000000;

bool regionsFit(const std::uint8_t* d, const Layout& layout)
{
    const auto& phdr = layout.tables[Phdr];
    const auto& pbag = layout.tables[Pbag];
    const auto& pgen = layout.tables[Pgen];
    const auto& inst = layout.tables[Inst];
    const auto& ibag = layout.tables[Ibag];
    const auto& igen = layout.tables[Igen];

    // The index each record holds into the next table down: a preset's first zone, a zone's first
    // generator, an instrument's first zone.
    const auto indexAt = [d](const Table& t, std::size_t i, std::size_t field) { return static_cast<std::size_t>(le16(d + t.at + i * t.recordSize + field)); };
    const auto genAt = [d](const Table& t, std::size_t i) { return d + t.at + i * 4; };

    // What the file has asked for so far. Both only grow by one, and are checked at once, so
    // neither can overflow; once either limit is passed, every loop below ends and the font is
    // refused.
    std::uint64_t regions = 0, steps = 0;
    const auto overBudget = [&] { return regions > kMaxRegions || steps > kMaxSteps; };

    // The first count, in tsf_load_presets: every zone with its own ranges.
    const auto counted = [&](std::size_t preset)
    {
        std::size_t count = 0;
        for (std::size_t b = indexAt(phdr, preset, 24); b < indexAt(phdr, preset + 1, 24); ++b)
        {
            Ranges p;
            for (std::size_t g = indexAt(pbag, b, 0); g < indexAt(pbag, b + 1, 0); ++g)
            {
                const std::uint8_t* gen = genAt(pgen, g);
                if (le16(gen) == 43 || le16(gen) == 44)
                {
                    p.apply(gen);
                    continue;
                }
                if (le16(gen) != 41 || static_cast<std::size_t>(le16(gen + 2)) >= inst.count)
                    continue;
                const std::size_t i = static_cast<std::size_t>(le16(gen + 2));
                for (std::size_t zb = indexAt(inst, i, 20); zb < indexAt(inst, i + 1, 20); ++zb)
                {
                    Ranges z;
                    for (std::size_t zg = indexAt(ibag, zb, 0); zg < indexAt(ibag, zb + 1, 0); ++zg)
                    {
                        if (++steps > kMaxSteps)
                            return count;
                        const std::uint8_t* zgen = genAt(igen, zg);
                        if (le16(zgen) == 43 || le16(zgen) == 44)
                            z.apply(zgen);
                        else if (le16(zgen) == 53 && z.hikey >= p.lokey && z.lokey <= p.hikey && z.hivel >= p.lovel && z.lovel <= p.hivel)
                        {
                            ++count;
                            if (++regions > kMaxRegions)
                                return count;
                        }
                    }
                    if (++steps > kMaxSteps)
                        return count;
                }
            }
        }
        return count;
    };

    // The second pass: ranges inherited from global zones, and zones narrowed to the preset's.
    const auto filled = [&](std::size_t preset)
    {
        std::size_t count = 0;
        Ranges global;
        const std::size_t firstBag = indexAt(phdr, preset, 24);
        for (std::size_t b = firstBag; b < indexAt(phdr, preset + 1, 24); ++b)
        {
            Ranges p = global;
            bool hadInstrument = false;
            for (std::size_t g = indexAt(pbag, b, 0); g < indexAt(pbag, b + 1, 0); ++g)
            {
                const std::uint8_t* gen = genAt(pgen, g);
                if (le16(gen) != 41)
                {
                    p.apply(gen);
                    continue;
                }
                if (static_cast<std::size_t>(le16(gen + 2)) >= inst.count)
                    continue;
                const std::size_t i = static_cast<std::size_t>(le16(gen + 2));
                Ranges instrumentGlobal;
                for (std::size_t zb = indexAt(inst, i, 20); zb < indexAt(inst, i + 1, 20); ++zb)
                {
                    if (++steps > kMaxSteps)
                        return count;
                    Ranges z = instrumentGlobal;
                    bool hadSample = false;
                    for (std::size_t zg = indexAt(ibag, zb, 0); zg < indexAt(ibag, zb + 1, 0); ++zg)
                    {
                        if (++steps > kMaxSteps)
                            return count;
                        const std::uint8_t* zgen = genAt(igen, zg);
                        if (le16(zgen) != 53)
                        {
                            z.apply(zgen);
                            continue;
                        }
                        if (z.hikey < p.lokey || z.lokey > p.hikey || z.hivel < p.lovel || z.lovel > p.hivel)
                            continue;
                        z.lokey = std::max(z.lokey, p.lokey);
                        z.hikey = std::min(z.hikey, p.hikey);
                        z.lovel = std::max(z.lovel, p.lovel);
                        z.hivel = std::min(z.hivel, p.hivel);
                        ++count;
                        hadSample = true;
                    }
                    if (zb == indexAt(inst, i, 20) && !hadSample)
                        instrumentGlobal = z;
                }
                hadInstrument = true;
            }
            if (b == firstBag && !hadInstrument)
                global = p;
        }
        return count;
    };

    for (std::size_t preset = 0; preset + 1 < phdr.count; ++preset)
    {
        const std::size_t allowed = counted(preset);
        if (overBudget())
            return false;
        const std::size_t made = filled(preset);
        if (overBudget() || made > allowed)
            return false;
    }
    return true;
}

// 20 log10(2) per unit of pan, so that a pan of 0.5 (hard to one side) is 6.02 dB.
constexpr float kPanPairDb = 12.04f;

// The positions in a region come from sums of numbers in the file, which the checks above cannot
// bound. A region that would read outside the sample data is shortened, or switched off.
void sanitizeRegions(tsf* font, std::uint32_t sampleCount)
{
    for (int p = 0; p < font->presetNum; ++p)
        for (int r = 0; r < font->presets[p].regionNum; ++r)
        {
            tsf_region& region = font->presets[p].regions[r];
            const std::uint32_t last = sampleCount > 0 ? sampleCount - 1 : 0; // the last frame TinySoundFont may read
            region.end = std::min(region.end, last);
            region.loop_end = std::min(region.loop_end, last);
            if (region.sample_rate == 0 || region.offset >= region.end)
            {
                region.lokey = 255; // matches no note
                region.hikey = 0;
            }
            if (region.pitch_keycenter < 0 || region.pitch_keycenter > 127)
                region.pitch_keycenter = 60;
            region.transpose = std::clamp(region.transpose, -240, 240);
            region.tune = std::clamp(region.tune, -1200, 1200);
            region.pitch_keytrack = std::clamp(region.pitch_keytrack, -1200, 1200);

            // TinySoundFont reads the format's centibels as a hundredth (0.01 at tsf.h:652) and plays them as
            // decibels, so 200 cB comes out as 2 dB rather than 20. The factor is in SoundFontBank.h.
            region.attenuation *= soundfont::kAttenuationFix;
            // Its mono output ignores the pan, so a stereo pair, one zone panned to each side, would play twice
            // as strong as a mono zone. The pan runs from -0.5 to 0.5; a hard-panned zone drops 6.02 dB, which
            // makes a correlated pair as loud as one mono zone, and a centred zone is left as it is.
            region.attenuation += kPanPairDb * std::abs(region.pan);
        }
}

struct Entry
{
    std::string name;
    int bank;
    int program;
};

// TinySoundFont's own limit of simultaneous voices, which it fills before the first note.
constexpr int kMaxVoices = 32;
constexpr int kScratchSamples = 1024;
} // namespace

struct SoundFontBank::Impl
{
    tsf* font = nullptr;
    int rate = 44100;
    std::vector<Entry> entries;
    std::vector<float> scratch = std::vector<float>(kScratchSamples);

    ~Impl() { tsf_close(font); }
};

SoundFontBank::SoundFontBank(std::unique_ptr<Impl> i) : impl(std::move(i)) {}

SoundFontBank::~SoundFontBank() = default;

std::unique_ptr<SoundFontBank> SoundFontBank::fromMemory(const void* data, int size, double sampleRate)
{
    if (data == nullptr || size < 12)
        return nullptr;

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    Layout layout;
    if (!Replay(bytes, static_cast<std::size_t>(size)).run(layout) || !checkIndexes(bytes, layout) || !regionsFit(bytes, layout))
        return nullptr;

    auto impl = std::make_unique<Impl>();
    impl->font = tsf_load_memory(data, size);
    if (impl->font == nullptr || impl->font->presetNum < 1)
        return nullptr;

    sanitizeRegions(impl->font, static_cast<std::uint32_t>(layout.sampleBytes / 2));

    // The voices and the channel are made now, because TinySoundFont makes them at the first request.
    impl->rate = std::max(1, static_cast<int>(std::lround(sampleRate)));
    tsf_set_output(impl->font, TSF_MONO, impl->rate, 0.0f);
    if (!tsf_set_max_voices(impl->font, kMaxVoices) || !tsf_channel_set_presetindex(impl->font, 0, 0)
        || !tsf_channel_set_pitchrange(impl->font, 0, 2.0f))
        return nullptr;

    for (int i = 0; i < impl->font->presetNum; ++i)
        impl->entries.push_back({ tsf_get_presetname(impl->font, i), impl->font->presets[i].bank, impl->font->presets[i].preset });

    return std::unique_ptr<SoundFontBank>(new SoundFontBank(std::move(impl)));
}

int SoundFontBank::instrumentCount() const { return static_cast<int>(impl->entries.size()); }

std::string SoundFontBank::instrumentName(int index) const
{
    return index >= 0 && index < instrumentCount() ? impl->entries[static_cast<std::size_t>(index)].name : std::string();
}

int SoundFontBank::bankNumber(int index) const
{
    return index >= 0 && index < instrumentCount() ? impl->entries[static_cast<std::size_t>(index)].bank : -1;
}

int SoundFontBank::programNumber(int index) const
{
    return index >= 0 && index < instrumentCount() ? impl->entries[static_cast<std::size_t>(index)].program : -1;
}

int SoundFontBank::indexOf(int bank, int program) const
{
    for (std::size_t i = 0; i < impl->entries.size(); ++i)
        if (impl->entries[i].bank == bank && impl->entries[i].program == program)
            return static_cast<int>(i);
    return -1;
}

void SoundFontBank::setSampleRate(double sampleRate)
{
    const int rate = std::max(1, static_cast<int>(std::lround(sampleRate)));
    if (rate == impl->rate)
        return;

    // The voices hold times in samples of the old rate.
    stopNotes();
    impl->rate = rate;
    tsf_set_output(impl->font, TSF_MONO, rate, 0.0f);
}

void SoundFontBank::selectInstrument(int index) { tsf_channel_set_presetindex(impl->font, 0, index); }

void SoundFontBank::noteOn(int note, float velocity) { tsf_channel_note_on(impl->font, 0, note, velocity); }

void SoundFontBank::noteOff(int note) { tsf_channel_note_off(impl->font, 0, note); }

void SoundFontBank::setPitchWheel(int position) { tsf_channel_set_pitchwheel(impl->font, 0, position); }

void SoundFontBank::setSustain(bool on) { tsf_channel_set_sustain(impl->font, 0, on ? 1 : 0); }

void SoundFontBank::stopNotes()
{
    if (tsf_active_voice_count(impl->font) == 0)
        return;

    // TinySoundFont ends a voice with a 10 ms release, and there is no call that skips it. So the
    // release is rendered and thrown away, which leaves the voices ended and the output untouched.
    tsf_channel_sounds_off_all(impl->font, 0);
    int remaining = impl->rate / 50 + kScratchSamples;
    while (remaining > 0 && tsf_active_voice_count(impl->font) > 0)
    {
        const int n = std::min(remaining, kScratchSamples);
        tsf_render_float(impl->font, impl->scratch.data(), n, 0);
        remaining -= n;
    }
}

int SoundFontBank::activeVoices() const { return tsf_active_voice_count(impl->font); }

void SoundFontBank::render(float* out, int numSamples) { tsf_render_float(impl->font, out, numSamples, 0); }
} // namespace retrochip
