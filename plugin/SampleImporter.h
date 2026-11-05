// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include "genesis/GenesisEngine.h"
#include "nes/Fti.h"
#include "snes/Brr.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace retrochip
{
// What the S-DSP's sample memory allows: 64 Ko of BRR, which is 7281 blocks of 9 bytes and 16 samples.
inline constexpr int kMaxSampleBlocks = 7281;
inline constexpr int kMaxSampleBytes = kMaxSampleBlocks * brr::kBytesPerBlock;
inline constexpr int kMaxSampleFrames = kMaxSampleBlocks * brr::kSamplesPerBlock;

// The state's properties for the user's sample. The root note and the loop are ordinary parameters.
inline constexpr const char* kSampleProperty = "snesSample";         // the BRR, in base64
inline constexpr const char* kSampleNameProperty = "snesSampleName";

struct ImportResult
{
    std::unique_ptr<SnesSample> sample; // null if the file could not be used
    juce::String message;               // why not, or what was cut; empty for a clean import
    bool truncated = false;
};

// Reads a WAV file, mixes it to mono, brings it to the chip's 32 kHz, and compresses it to BRR with the
// loop at the start. Slow and allocating: the message thread only, never the audio thread.
//
// Anything above 1 after resampling is scaled down to the encoder's range; quieter files keep their level.
// At most kMaxSampleBlocks blocks are kept, and truncated says when the file was longer.
ImportResult importWav(const juce::File& file);

// The sample's BRR for the state, and back. Decoding checks the size (a multiple of 9, not over 64 Ko) and
// returns null for anything it cannot use.
juce::String sampleToBase64(const SnesSample& sample);
std::unique_ptr<SnesSample> sampleFromBase64(const juce::String& base64, const juce::String& name);

// The YM2612's imports. There is no memory on the chip to set a limit, so this one keeps the state of a project
// reasonable: about 20 s at the DAC's rate.
inline constexpr int kMaxPcmBytes = 256 * 1024;

// The state's properties for them. Each is the data in base64, with its name beside it.
inline constexpr const char* kGenesisTfiProperty = "genTfi";
inline constexpr const char* kGenesisTfiNameProperty = "genTfiName";
inline constexpr const char* kGenesisPcmProperty = "genPcm";
inline constexpr const char* kGenesisPcmNameProperty = "genPcmName";

// A kit goes up the keys from the lowest the keyboard shows, for as many as it shows. Its samples share kMaxPcmBytes.
inline constexpr int kFirstKitNote = 36;
inline constexpr int kMaxKitSamples = 61;

// A kit is in the state as a child tree, with one child for each sample.
inline constexpr const char* kGenesisKitTree = "GENKIT";
inline constexpr const char* kGenesisKitSampleTree = "SAMPLE";
inline constexpr const char* kGenesisKitNoteProperty = "note";
inline constexpr const char* kGenesisKitNameProperty = "name";
inline constexpr const char* kGenesisKitDataProperty = "data";

struct TfiImport
{
    std::optional<FmPatch> patch; // empty if the file could not be used
    std::array<std::uint8_t, kTfiSize> file {};
    std::string name;
    juce::String message; // why not
};

struct KitImport
{
    std::vector<PcmKitSample> samples; // empty if no file could be used
    juce::String message;              // what was left out or cut; empty for a clean import
};

struct PcmImport
{
    std::vector<std::uint8_t> pcm; // empty if the file could not be used
    std::string name;
    juce::String message;          // why not, or what was cut; empty for a clean import
    bool truncated = false;
};

// The NES's imports. An .fti file is read whole, so it has a limit; a 2A03 instrument with 64 full samples is under it.
inline constexpr int kMaxFtiBytes = 1024 * 1024;
inline constexpr const char* kNesFtiProperties[3] = { "nesFti2a03", "nesFtiVrc6", "nesFtiN163" }; // by NesChip
inline constexpr const char* kNesDpcmProperty = "nesDpcm";
inline constexpr const char* kNesDpcmNameProperty = "nesDpcmName";

struct FtiImport
{
    std::optional<FtiInstrument> instrument; // empty if the file could not be used
    std::vector<std::uint8_t> file;
    juce::String message;
};

struct DpcmImport
{
    std::vector<std::uint8_t> bytes; // empty if the file could not be used
    std::string name;
    juce::String message; // why not, or what was cut
    bool truncated = false;
};

// Reads a FamiTracker instrument.
FtiImport importFti(const juce::File& file);

// The instrument in a file's bytes, as importFti() reads them; for the state.
std::optional<FtiInstrument> ftiFromBytes(const std::vector<std::uint8_t>& bytes);

// Reads a WAV file, mixes it to mono, brings it to the delta-modulation channel's fastest rate, and encodes it. At most
// dpcm::kMaxBytes, about one second; anything above 1 is scaled down first.
DpcmImport importDpcm(const juce::File& file);

// Reads a timbre. The message thread only, like what follows.
TfiImport importTfi(const juce::File& file);

// Reads a WAV file, mixes it to mono, brings it to the DAC's rate, and rounds it to 8 bits. Anything above 1 after
// resampling is scaled down; quieter files keep their level. At most kMaxPcmBytes are kept.
PcmImport importPcm(const juce::File& file);

// Reads the files as importPcm() does, sorted by name as a person would read them, and puts them on the keys one
// after the other. The files it cannot use are left out and leave no gap.
KitImport importKit(const juce::Array<juce::File>& files);

// The imports' data for the state, and back. Decoding returns nothing for anything it cannot use.
juce::String bytesToBase64(const std::uint8_t* data, size_t size);
std::vector<std::uint8_t> bytesFromBase64(const juce::String& base64, size_t maxBytes);
} // namespace retrochip
