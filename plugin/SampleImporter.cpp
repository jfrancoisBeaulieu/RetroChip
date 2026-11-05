// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "SampleImporter.h"

#include "Localisation.h"

#include "Resampler.h"
#include "genesis/Tfi.h"
#include "nes/Dpcm.h"
#include "snes/BrrEncoder.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace retrochip
{
namespace
{
constexpr double kChipRate = 32000.0;
constexpr int kReadChunk = 16384;
constexpr unsigned int kMaxChannels = 64;
constexpr double kMinSampleRate = 1000.0;
constexpr double kMaxSampleRate = 768000.0;
constexpr int kPullChunk = 4096;


ImportResult failure(const juce::String& message)
{
    ImportResult result;
    result.message = message;
    return result;
}

// Mono mix of the first `count` frames of the file, with anything that is not a number as silence.
bool readMono(juce::AudioFormatReader& reader, juce::int64 count, std::vector<float>& mono)
{
    const int channels = static_cast<int>(reader.numChannels);
    juce::AudioBuffer<float> buffer(channels, kReadChunk);
    mono.assign(static_cast<size_t>(count), 0.0f);

    for (juce::int64 at = 0; at < count; at += kReadChunk)
    {
        const int n = static_cast<int>(std::min<juce::int64>(kReadChunk, count - at));
        if (!reader.read(buffer.getArrayOfWritePointers(), channels, at, n))
            return false;
        for (int i = 0; i < n; ++i)
        {
            float sum = 0.0f;
            for (int c = 0; c < channels; ++c)
                sum += buffer.getSample(c, i);
            const float mixed = sum / static_cast<float>(channels);
            mono[static_cast<size_t>(at) + static_cast<size_t>(i)] = std::isfinite(mixed) ? mixed : 0.0f;
        }
    }
    return true;
}

// Brings the first `period` mono samples from `rate` to `outputRate`, producing `wanted` of them. The filter
// reads around the ends of the period, as a loop will, so that the first and last samples keep their level.
std::vector<float> resample(const std::vector<float>& in, size_t period, double rate, double outputRate, size_t wanted)
{
    std::vector<float> out;
    if (wanted == period && std::abs(rate - outputRate) < 0.5)
    {
        out.assign(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(period));
        return out;
    }

    auto resampler = std::make_unique<Resampler>(); // about 200 KB
    resampler->prepare(rate, outputRate);

    // The lead-in fills the filter, and the output it causes is dropped along with the filter's delay.
    constexpr size_t leadIn = Resampler::kTaps;
    const size_t dropped = static_cast<size_t>(resampler->latencyOutputSamples())
                           + static_cast<size_t>(std::llround(static_cast<double>(leadIn) * outputRate / rate));
    const size_t target = wanted + dropped;

    out.reserve(target);
    std::vector<float> pulled(kPullChunk);
    std::vector<float> pushed(Resampler::kMaxChunk);
    size_t fed = period * (leadIn / period + 1) - leadIn; // where the lead-in starts in the period, going round
    for (int stalls = 0; out.size() < target && stalls < 8;)
    {
        const int want = static_cast<int>(std::min<size_t>(kPullChunk, target - out.size()));
        int needed = resampler->inputNeededFor(want);
        while (needed > 0)
        {
            const int n = std::min(needed, Resampler::kMaxChunk);
            for (int i = 0; i < n; ++i)
                pushed[static_cast<size_t>(i)] = in[(fed + static_cast<size_t>(i)) % period];
            resampler->push(pushed.data(), n);
            fed += static_cast<size_t>(n);
            needed -= n;
        }
        const int got = resampler->pull(pulled.data(), want);
        out.insert(out.end(), pulled.begin(), pulled.begin() + got);
        stalls = got == 0 ? stalls + 1 : 0;
    }

    out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(std::min(dropped, out.size())));
    out.resize(wanted, 0.0f);
    return out;
}

// The sample fills whole blocks, because the encoder would pad the rest with silence, which a loop would play.
// A length that isn't a whole number of blocks is stretched to the nearest one, moving the pitch very slightly.
size_t lengthInWholeBlocks(double frames)
{
    constexpr double samplesPerBlock = brr::kSamplesPerBlock;
    if (frames < samplesPerBlock)
        return std::max<size_t>(1, static_cast<size_t>(std::llround(frames))); // too short to stretch: padded
    return static_cast<size_t>(std::llround(frames / samplesPerBlock)) * brr::kSamplesPerBlock;
}

// Only ever brings the level down, and only when it is over what the encoder takes.
void limitPeak(std::vector<float>& samples)
{
    float peak = 0.0f;
    for (const float s : samples)
        peak = std::max(peak, std::abs(s));
    if (peak > 1.0f)
        for (auto& s : samples)
            s /= peak;
}

// The number and its unit stay on one line of the status label.
juce::String seconds(double value)
{
    return oneDecimal(value);
}

// Null for a file that cannot be used, with the reason in `message`.
std::unique_ptr<juce::AudioFormatReader> openWav(const juce::File& file, juce::String& message)
{
    const auto unreadable = tr("Unreadable file: ") + file.getFileName();
    const auto empty = tr("Empty file: ") + file.getFileName();

    if (!file.existsAsFile())
    {
        message = unreadable;
        return nullptr;
    }
    if (file.getSize() == 0)
    {
        message = empty;
        return nullptr;
    }

    juce::WavAudioFormat format;
    auto stream = file.createInputStream(); // null when the file exists but cannot be opened
    if (stream == nullptr)
    {
        message = unreadable;
        return nullptr;
    }
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(stream.release(), true));

    // A damaged header can claim anything; what a real recording has is well inside these.
    if (reader == nullptr || reader->numChannels == 0 || reader->numChannels > kMaxChannels
        || !(reader->sampleRate >= kMinSampleRate && reader->sampleRate <= kMaxSampleRate))
    {
        message = unreadable;
        return nullptr;
    }
    if (reader->lengthInSamples <= 0)
    {
        message = empty;
        return nullptr;
    }
    return reader;
}

// importPcm() with a limit of its own. The message for a cut is the caller's.
PcmImport readPcm(const juce::File& file, int maxBytes)
{
    PcmImport result;
    const auto reader = openWav(file, result.message);
    if (reader == nullptr)
        return result;

    // Only the part that fits is read. The length is rounded, moving the pitch by less than one frame's worth.
    const double chipRate = GenesisEngine::kPcmRate;
    const double rate = reader->sampleRate;
    const auto outputLength = static_cast<double>(reader->lengthInSamples) * chipRate / rate;
    result.truncated = outputLength > static_cast<double>(maxBytes) + 0.5;
    const size_t wanted = result.truncated ? static_cast<size_t>(maxBytes)
                                           : std::max<size_t>(1, static_cast<size_t>(std::llround(outputLength)));
    const juce::int64 inputUsed = result.truncated ? static_cast<juce::int64>(std::llround(static_cast<double>(wanted) * rate / chipRate))
                                                   : reader->lengthInSamples;
    std::vector<float> mono;
    if (!readMono(*reader, inputUsed, mono))
    {
        result.message = tr("Unreadable file: ") + file.getFileName();
        result.truncated = false;
        return result;
    }

    const double outputRate = result.truncated ? chipRate : chipRate * static_cast<double>(wanted) / outputLength;
    auto samples = resample(mono, mono.size(), rate, outputRate, wanted);
    limitPeak(samples);

    result.pcm.resize(samples.size());
    std::transform(samples.begin(), samples.end(), result.pcm.begin(), [](float s)
                   { return static_cast<std::uint8_t>(std::clamp(128L + std::lround(s * 127.0f), 1L, 255L)); });
    result.name = file.getFileNameWithoutExtension().toStdString();
    return result;
}
} // namespace

ImportResult importWav(const juce::File& file)
{
    juce::String problem;
    const auto reader = openWav(file, problem);
    if (reader == nullptr)
        return failure(problem);

    // Only the part that fits is read, with a little more for the filter to see past the cut.
    const double rate = reader->sampleRate;
    const auto outputLength = static_cast<double>(reader->lengthInSamples) * kChipRate / rate;
    const bool truncated = outputLength > static_cast<double>(kMaxSampleFrames) + 0.5;
    const size_t wanted = truncated ? static_cast<size_t>(kMaxSampleFrames) : lengthInWholeBlocks(outputLength);
    const juce::int64 inputUsed = truncated ? static_cast<juce::int64>(std::llround(static_cast<double>(wanted) * rate / kChipRate))
                                            : reader->lengthInSamples;
    std::vector<float> mono;
    if (!readMono(*reader, inputUsed, mono))
        return failure(tr("Unreadable file: ") + file.getFileName());

    const bool padded = !truncated && static_cast<double>(wanted) < brr::kSamplesPerBlock;
    const double outputRate = truncated || padded ? kChipRate : kChipRate * static_cast<double>(wanted) / outputLength;
    auto samples = resample(mono, mono.size(), rate, outputRate, wanted);
    limitPeak(samples);

    ImportResult result;
    result.sample = std::make_unique<SnesSample>();
    result.sample->brr = encodeBrr(samples, 0); // the loop starts at the first block, where snesLoop restarts it
    result.sample->loopBlock = brrLoopBlockFor(0);
    result.sample->name = file.getFileNameWithoutExtension().toStdString();
    result.truncated = truncated;
    if (truncated)
        result.message = tr("Sample cut at ") + seconds(static_cast<double>(kMaxSampleFrames) / kChipRate)
                         + tr("\u00a0s (64 KB limit)");
    return result;
}

TfiImport importTfi(const juce::File& file)
{
    TfiImport result;
    if (!file.existsAsFile())
    {
        result.message = tr("Unreadable file: ") + file.getFileName();
        return result;
    }

    // The size first: nothing longer than a timbre is read.
    juce::MemoryBlock bytes;
    if (file.getSize() == static_cast<juce::int64>(kTfiSize) && file.loadFileAsData(bytes))
        result.patch = parseTfi(static_cast<const std::uint8_t*>(bytes.getData()), bytes.getSize());
    if (!result.patch)
    {
        result.message = tr("Invalid TFI file: ") + file.getFileName();
        return result;
    }
    std::copy_n(static_cast<const std::uint8_t*>(bytes.getData()), kTfiSize, result.file.begin());
    result.name = file.getFileNameWithoutExtension().toStdString();
    return result;
}

PcmImport importPcm(const juce::File& file)
{
    auto result = readPcm(file, kMaxPcmBytes);
    if (result.truncated)
        result.message = tr("Sample cut at ") + seconds(static_cast<double>(kMaxPcmBytes) / GenesisEngine::kPcmRate)
                         + tr("\u00a0s (256 KB limit)");
    return result;
}

KitImport importKit(const juce::Array<juce::File>& files)
{
    auto sorted = files;
    std::sort(sorted.begin(), sorted.end(), [](const juce::File& a, const juce::File& b)
              { return a.getFileName().compareNatural(b.getFileName()) < 0; });

    KitImport result;
    juce::StringArray unusable;
    int bytesLeft = kMaxPcmBytes;
    bool cut = false;
    bool tooMany = false;
    for (const auto& file : sorted)
    {
        if (static_cast<int>(result.samples.size()) == kMaxKitSamples)
        {
            tooMany = true;
            break;
        }
        if (bytesLeft == 0)
        {
            cut = true;
            break;
        }
        if (!file.hasFileExtension("wav;wave"))
        {
            unusable.add(file.getFileName());
            continue;
        }

        auto pcm = readPcm(file, bytesLeft);
        if (pcm.pcm.empty())
        {
            unusable.add(file.getFileName());
            continue;
        }
        cut = cut || pcm.truncated;
        bytesLeft -= static_cast<int>(pcm.pcm.size());
        result.samples.push_back({ kFirstKitNote + static_cast<int>(result.samples.size()), std::move(pcm.pcm), std::move(pcm.name) });
    }

    juce::StringArray notes;
    if (!unusable.isEmpty())
        notes.add(tr("Skipped: ") + unusable.joinIntoString(", "));
    if (cut)
        notes.add(tr("Kit cut (256 KB limit)"));
    if (tooMany)
        notes.add(tr("Kit limited to ") + juce::String(kMaxKitSamples) + tr(" sounds"));
    result.message = notes.joinIntoString(" · ");
    return result;
}

FtiImport importFti(const juce::File& file)
{
    FtiImport result;
    if (!file.existsAsFile())
    {
        result.message = tr("Unreadable file: ") + file.getFileName();
        return result;
    }
    juce::MemoryBlock bytes;
    if (file.getSize() > kMaxFtiBytes || !file.loadFileAsData(bytes))
    {
        result.message = tr("Unreadable file: ") + file.getFileName();
        return result;
    }

    const auto* data = static_cast<const std::uint8_t*>(bytes.getData());
    const auto parsed = parseFti(data, bytes.getSize());
    switch (parsed.error)
    {
        case FtiError::none:
            result.instrument = parsed.instrument;
            result.file.assign(data, data + bytes.getSize());
            break;
        case FtiError::unsupportedChip:
            result.message = tr("Unsupported chip (VRC7, FDS or 5B): ") + file.getFileName();
            break;
        case FtiError::damaged:
            result.message = tr("Damaged FTI file: ") + file.getFileName();
            break;
        case FtiError::notFti:
        case FtiError::unsupportedVersion:
            result.message = tr("Invalid FTI file: ") + file.getFileName();
            break;
    }
    return result;
}

std::optional<FtiInstrument> ftiFromBytes(const std::vector<std::uint8_t>& bytes)
{
    return parseFti(bytes.data(), bytes.size()).instrument;
}

DpcmImport importDpcm(const juce::File& file)
{
    DpcmImport result;
    const auto reader = openWav(file, result.message);
    if (reader == nullptr)
        return result;

    // Only what fits is read.
    const double chipRate = dpcm::rateHz(dpcm::kFastestRate);
    constexpr std::size_t maxSamples = static_cast<std::size_t>(dpcm::kMaxBytes) * 8;
    const double rate = reader->sampleRate;
    const auto outputLength = static_cast<double>(reader->lengthInSamples) * chipRate / rate;
    result.truncated = outputLength > static_cast<double>(maxSamples) + 0.5;
    const std::size_t wanted = result.truncated ? maxSamples : std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(outputLength)));
    const juce::int64 inputUsed = result.truncated ? static_cast<juce::int64>(std::llround(static_cast<double>(wanted) * rate / chipRate))
                                                   : reader->lengthInSamples;
    std::vector<float> mono;
    if (!readMono(*reader, inputUsed, mono))
    {
        result.message = tr("Unreadable file: ") + file.getFileName();
        result.truncated = false;
        return result;
    }

    auto samples = resample(mono, mono.size(), rate, result.truncated ? chipRate : chipRate * static_cast<double>(wanted) / outputLength, wanted);
    limitPeak(samples);
    result.bytes = dpcm::encode(samples);
    result.name = file.getFileNameWithoutExtension().toStdString();
    if (result.truncated)
        result.message = tr("Sample cut at ") + seconds(static_cast<double>(maxSamples) / chipRate)
                         + tr("\u00a0s (DPCM limit: 4081 bytes)");
    return result;
}

juce::String bytesToBase64(const std::uint8_t* data, size_t size)
{
    return juce::Base64::toBase64(data, size);
}

std::vector<std::uint8_t> bytesFromBase64(const juce::String& base64, size_t maxBytes)
{
    // Four characters hold three bytes: refuse what is too big before decoding any of it.
    if (base64.isEmpty() || static_cast<size_t>(base64.length()) > (maxBytes + 2) / 3 * 4)
        return {};

    juce::MemoryOutputStream decoded;
    if (!juce::Base64::convertFromBase64(decoded, base64) || decoded.getDataSize() > maxBytes)
        return {};
    const auto* data = static_cast<const std::uint8_t*>(decoded.getData());
    return { data, data + decoded.getDataSize() };
}

juce::String sampleToBase64(const SnesSample& sample)
{
    return juce::Base64::toBase64(sample.brr.data(), sample.brr.size());
}

std::unique_ptr<SnesSample> sampleFromBase64(const juce::String& base64, const juce::String& name)
{
    // Four characters hold three bytes: refuse what is too big before decoding any of it.
    if (base64.isEmpty() || base64.length() > (kMaxSampleBytes + 2) / 3 * 4)
        return nullptr;

    juce::MemoryOutputStream decoded;
    if (!juce::Base64::convertFromBase64(decoded, base64))
        return nullptr;
    const auto size = decoded.getDataSize();
    if (size == 0 || size > static_cast<size_t>(kMaxSampleBytes) || size % brr::kBytesPerBlock != 0)
        return nullptr;

    auto sample = std::make_unique<SnesSample>();
    const auto* data = static_cast<const std::uint8_t*>(decoded.getData());
    sample->brr.assign(data, data + size);
    sample->loopBlock = brrLoopBlockFor(0);
    sample->name = name.toStdString();
    return sample;
}
} // namespace retrochip
