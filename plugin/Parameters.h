// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include "Localisation.h"
#include "genesis/FmPatches.h"
#include "genesis/GenesisEngine.h"
#include "nes/N163Waves.h"
#include "snes/SnesBank.h"
#include "snes/SnesEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>

namespace retrochip::ids
{
inline constexpr const char* channel = "channel";
inline constexpr const char* duty = "duty";
inline constexpr const char* noiseMode = "noiseMode";
inline constexpr const char* attack = "attack";
inline constexpr const char* decay = "decay";
inline constexpr const char* sustain = "sustain";
inline constexpr const char* release = "release";
inline constexpr const char* vibratoRate = "vibratoRate";
inline constexpr const char* vibratoDepth = "vibratoDepth";
inline constexpr const char* glide = "glide";
inline constexpr const char* velocity = "velocity";
inline constexpr const char* gain = "gain";
inline constexpr const char* console = "console";
inline constexpr const char* genPatch = "genPatch";
inline constexpr const char* genAlgorithm = "genAlgorithm";
inline constexpr const char* genFeedback = "genFeedback";
inline constexpr const char* genBrightness = "genBrightness";
inline constexpr const char* genAttack = "genAttack";
inline constexpr const char* genDecay = "genDecay";
inline constexpr const char* genRelease = "genRelease";
inline constexpr const char* genRootNote = "genRootNote";
inline constexpr const char* genLoop = "genLoop";
inline constexpr const char* snesInstrument = "snesInstrument";
inline constexpr const char* snesAttack = "snesAttack";
inline constexpr const char* snesDecay = "snesDecay";
inline constexpr const char* snesSustainLevel = "snesSustainLevel";
inline constexpr const char* snesSustainRate = "snesSustainRate";
inline constexpr const char* snesRelease = "snesRelease";
inline constexpr const char* snesEchoMix = "snesEchoMix";
inline constexpr const char* snesEchoDelay = "snesEchoDelay";
inline constexpr const char* snesEchoFeedback = "snesEchoFeedback";
inline constexpr const char* snesRootNote = "snesRootNote";
inline constexpr const char* snesLoop = "snesLoop";
inline constexpr const char* sfEchoMix = "sfEchoMix";
inline constexpr const char* sfEchoDelay = "sfEchoDelay";
inline constexpr const char* sfEchoFeedback = "sfEchoFeedback";
inline constexpr const char* psgChannel = "psgChannel";
inline constexpr const char* psgNoiseType = "psgNoiseType";
inline constexpr const char* psgNoiseRate = "psgNoiseRate";
inline constexpr const char* psgAttack = "psgAttack";
inline constexpr const char* psgDecay = "psgDecay";
inline constexpr const char* psgSustain = "psgSustain";
inline constexpr const char* psgRelease = "psgRelease";
inline constexpr const char* nesUseInstrument = "nesUseInstrument";
inline constexpr const char* vrc6Channel = "vrc6Channel";
inline constexpr const char* vrc6Duty = "vrc6Duty";
inline constexpr const char* n163Wave = "n163Wave";
inline constexpr const char* n163Channels = "n163Channels";
inline constexpr const char* dpcmRootNote = "dpcmRootNote";
inline constexpr const char* dpcmLoop = "dpcmLoop";
} // namespace retrochip::ids

namespace retrochip
{
// The order matches the choices of the console parameter, which show the consoles' names. The enum names the chips.
enum class Console
{
    Chip2A03,
    Ym2612,
    SDsp,
    SoundFont,
    Psg,
    Vrc6,
    N163
};
} // namespace retrochip

namespace retrochip
{
// The 12 instruments, built on first use and then shared by every plugin instance, because a build
// takes about 250 ms. The first call happens on the thread that makes the first plugin, never the audio
// thread. The engines keep a pointer to it, so it lives until the process ends.
inline const SnesEngine::Bank& sharedSnesBank()
{
    static const SnesEngine::Bank bank = buildSnesBank();
    return bank;
}

// What the S-DSP's knobs show for each setting, from the chip's tables. Times are the ones fullsnes gives.
namespace snesText
{
inline constexpr const char* kAttack[16] = { "4.1 s", "2.5 s", "1.5 s", "1.0 s", "640 ms", "380 ms", "260 ms", "160 ms",
                                             "96 ms", "64 ms", "40 ms", "24 ms", "16 ms", "10 ms", "6 ms", "0 ms" };
inline constexpr const char* kDecay[8] = { "1.2 s", "740 ms", "440 ms", "290 ms", "180 ms", "110 ms", "74 ms", "37 ms" };
inline constexpr const char* kSustainRate[32] = {
    "Infinite", "38 s",   "28 s",   "24 s",   "19 s",   "14 s",   "12 s",   "9.4 s",  "7.1 s",  "5.9 s",  "4.7 s",
    "3.5 s",  "2.9 s",  "2.4 s",  "1.8 s",  "1.5 s",  "1.2 s",  "880 ms", "740 ms", "590 ms", "440 ms", "370 ms",
    "290 ms", "220 ms", "180 ms", "150 ms", "110 ms", "92 ms",  "74 ms",  "55 ms",  "37 ms",  "18 ms" };
inline constexpr const char* kUserInstrumentName = "My sample";
inline constexpr int kDefaultRootNote = 60;

// A note typed into the root note box: a name as the box shows it ("C4", "C#4", "Bb3", with middle C at
// octave 4, and octave 4 if none is given), or a note number. Anything else is the default.
inline int noteFromText(const juce::String& typed)
{
    const auto text = typed.trim();
    if (text.isEmpty())
        return kDefaultRootNote;

    const auto first = text[0];
    if (juce::CharacterFunctions::isDigit(first) || first == '-' || first == '+')
        return juce::jlimit(0, 127, text.getIntValue());

    const int letter = juce::String("cdefgab").indexOfChar(juce::CharacterFunctions::toLowerCase(first));
    if (letter < 0)
        return kDefaultRootNote;
    constexpr int kSemitones[] = { 0, 2, 4, 5, 7, 9, 11 };
    int note = kSemitones[letter];

    auto octave = text.substring(1);
    if (octave.startsWithChar('#'))
        ++note;
    else if (octave.startsWithChar('b'))
        --note;
    if (octave.startsWithChar('#') || octave.startsWithChar('b'))
        octave = octave.substring(1);

    if (octave.isNotEmpty() && !octave.containsOnly("-0123456789"))
        return kDefaultRootNote;
    return juce::jlimit(0, 127, note + 12 * ((octave.isEmpty() ? 4 : octave.getIntValue()) + 1));
}
} // namespace snesText

inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using juce::AudioParameterBool;
    using juce::AudioParameterChoice;
    using juce::AudioParameterFloat;
    using juce::AudioParameterFloatAttributes;
    using juce::AudioParameterInt;
    using juce::AudioParameterIntAttributes;
    using juce::NormalisableRange;
    using juce::ParameterID;

    const auto withUnit = [](int decimals, const juce::String& unit)
    {
        return AudioParameterFloatAttributes().withLabel(unit).withStringFromValueFunction(
            [decimals, unit](float value, int) { return juce::String(value, decimals) + " " + unit; });
    };

    const auto time = [&](const char* id, const char* name, float maximum, float defaultValue)
    {
        return std::make_unique<AudioParameterFloat>(ParameterID { id, 1 }, name,
                                                     NormalisableRange<float>(0.0f, maximum, 1.0f, 0.4f),
                                                     defaultValue, withUnit(0, "ms"));
    };

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ids::channel, 1 }, "Channel",
                                                      juce::StringArray { "Pulse", "Triangle", "Noise", "DPCM" }, 0));
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ids::duty, 1 }, "Duty",
                                                      juce::StringArray { "12.5%", "25%", "50%", "75%" }, 2));
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ids::noiseMode, 1 }, "Noise Mode",
                                                      juce::StringArray { "Long", "Short" }, 0));
    layout.add(time(ids::attack, "Attack", 2000.0f, 0.0f));
    layout.add(time(ids::decay, "Decay", 2000.0f, 0.0f));
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID { ids::sustain, 1 }, "Sustain",
                                                     NormalisableRange<float>(0.0f, 15.0f, 1.0f), 15.0f));
    layout.add(time(ids::release, "Release", 2000.0f, 50.0f));
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID { ids::vibratoRate, 1 }, "Vibrato Rate",
                                                     NormalisableRange<float>(0.5f, 12.0f, 0.1f), 5.0f,
                                                     withUnit(1, "Hz")));
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID { ids::vibratoDepth, 1 }, "Vibrato Depth",
                                                     NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f,
                                                     withUnit(0, "ct")));
    layout.add(time(ids::glide, "Glide", 1000.0f, 0.0f));
    layout.add(std::make_unique<AudioParameterBool>(ParameterID { ids::velocity, 1 }, "Velocity", true));
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID { ids::gain, 1 }, "Gain",
                                                     NormalisableRange<float>(-36.0f, 6.0f, 0.1f), 0.0f,
                                                     withUnit(1, "dB")));
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ids::console, 1 }, "Console",
                                                      juce::StringArray { "NES", "Genesis FM", "SNES", "SNES SoundFont", "Genesis PSG", "NES VRC6", "NES N163" }, 0));

    // The timbres, then what the user imports: see GenesisEngine::kUserPatch and kPcmPatch. The choices are in
    // English; the host and the editor show the user's own in the user's language.
    juce::StringArray patchNames;
    for (const auto& patch : kFmPatches)
        patchNames.add(patch.name);
    patchNames.add("My TFI timbre");
    patchNames.add("My PCM sample");
    patchNames.add("My PCM kit");
    layout.add(std::make_unique<AudioParameterChoice>(
        ParameterID { ids::genPatch, 1 }, "Timbre", patchNames, 0,
        juce::AudioParameterChoiceAttributes().withStringFromValueFunction(
            [patchNames](int index, int)
            {
                const auto& name = patchNames[juce::jlimit(0, patchNames.size() - 1, index)];
                return index >= GenesisEngine::kUserPatch ? tr(name.toRawUTF8()) : name;
            })));
    // Shown from 1 to 8. The engine counts algorithms from 0.
    layout.add(std::make_unique<AudioParameterInt>(ParameterID { ids::genAlgorithm, 1 }, "Algorithm", 1, 8, 5));
    layout.add(std::make_unique<AudioParameterInt>(ParameterID { ids::genFeedback, 1 }, "Feedback", 0, 7, 0));

    const auto macro = [&](const char* id, const char* name)
    {
        return std::make_unique<AudioParameterFloat>(ParameterID { id, 1 }, name,
                                                     NormalisableRange<float>(-100.0f, 100.0f, 1.0f), 0.0f,
                                                     withUnit(0, "%"));
    };
    layout.add(macro(ids::genBrightness, "Brightness"));
    layout.add(macro(ids::genAttack, "FM Attack"));
    layout.add(macro(ids::genDecay, "FM Decay"));
    layout.add(macro(ids::genRelease, "FM Release"));

    // The S-DSP. The instrument choices are the bank's, in its order, then the user's sample.
    juce::StringArray instrumentNames;
    for (const auto& sample : sharedSnesBank())
        instrumentNames.add(juce::String::fromUTF8(sample->name.c_str()));
    instrumentNames.add(snesText::kUserInstrumentName);
    layout.add(std::make_unique<AudioParameterChoice>(
        ParameterID { ids::snesInstrument, 1 }, "Instrument", instrumentNames, 0,
        juce::AudioParameterChoiceAttributes().withStringFromValueFunction(
            [instrumentNames](int index, int) { return tr(instrumentNames[juce::jlimit(0, instrumentNames.size() - 1, index)].toRawUTF8()); })));

    // An integer parameter shown through one of the chip's tables. A bare number typed into it counts in
    // units of numberPerStep.
    const auto chipInt = [](const char* id, const char* name, int maximum, int defaultValue,
                            std::function<juce::String(int)> text, int numberPerStep = 1)
    {
        return std::make_unique<AudioParameterInt>(
            ParameterID { id, 1 }, name, 0, maximum, defaultValue,
            AudioParameterIntAttributes()
                .withStringFromValueFunction([text](int value, int) { return text(value); })
                .withValueFromStringFunction(
                    [text, maximum, numberPerStep](const juce::String& typed)
                    {
                        // Typing a shown value picks that step; anything else is read as a number.
                        for (int i = 0; i <= maximum; ++i)
                            if (text(i).equalsIgnoreCase(typed.trim()))
                                return i;
                        return juce::jlimit(0, maximum, juce::roundToInt(typed.getIntValue() / static_cast<double>(numberPerStep)));
                    }));
    };
    layout.add(chipInt(ids::snesAttack, "S-DSP Attack", 15, 15, [](int i) { return juce::String(snesText::kAttack[juce::jlimit(0, 15, i)]); }));
    layout.add(chipInt(ids::snesDecay, "S-DSP Decay", 7, 7, [](int i) { return juce::String(snesText::kDecay[juce::jlimit(0, 7, i)]); }));
    layout.add(chipInt(ids::snesSustainLevel, "S-DSP Sustain Level", 7, 7,
                       [](int i) { return juce::String(juce::jlimit(0, 7, i) + 1) + "/8"; }));
    layout.add(chipInt(ids::snesSustainRate, "S-DSP Sustain Rate", 31, 0,
                       [](int i) { return i <= 0 ? tr(snesText::kSustainRate[0]) : juce::String(snesText::kSustainRate[juce::jlimit(0, 31, i)]); }));
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID { ids::snesRelease, 1 }, "S-DSP Release",
                                                     NormalisableRange<float>(8.0f, 2000.0f, 1.0f, 0.4f), 8.0f,
                                                     withUnit(0, "ms")));
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID { ids::snesEchoMix, 1 }, "Echo Mix",
                                                     NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f,
                                                     withUnit(0, "%")));
    constexpr int kEchoMsPerStep = 16;
    layout.add(chipInt(ids::snesEchoDelay, "Echo Delay", 15, 4, [](int i) { return juce::String(i * kEchoMsPerStep) + " ms"; }, kEchoMsPerStep));
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID { ids::snesEchoFeedback, 1 }, "Echo Feedback",
                                                     NormalisableRange<float>(0.0f, 100.0f, 1.0f), 30.0f,
                                                     withUnit(0, "%")));
    layout.add(std::make_unique<AudioParameterInt>(
        ParameterID { ids::snesRootNote, 1 }, "Root Note", 0, 127, snesText::kDefaultRootNote,
        AudioParameterIntAttributes()
            .withStringFromValueFunction([](int note, int) { return juce::MidiMessage::getMidiNoteName(note, true, true, 4); })
            .withValueFromStringFunction([](const juce::String& typed) { return snesText::noteFromText(typed); })));
    layout.add(std::make_unique<AudioParameterBool>(ParameterID { ids::snesLoop, 1 }, "Loop", false));

    // The SoundFont console's echo: the S-DSP's, with settings of its own.
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID { ids::sfEchoMix, 1 }, "SoundFont Echo Mix",
                                                     NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f,
                                                     withUnit(0, "%")));
    layout.add(chipInt(ids::sfEchoDelay, "SoundFont Echo Delay", 15, 4, [](int i) { return juce::String(i * kEchoMsPerStep) + " ms"; }, kEchoMsPerStep));
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID { ids::sfEchoFeedback, 1 }, "SoundFont Echo Feedback",
                                                     NormalisableRange<float>(0.0f, 100.0f, 1.0f), 30.0f,
                                                     withUnit(0, "%")));

    // The YM2612's PCM sample.
    layout.add(std::make_unique<AudioParameterInt>(
        ParameterID { ids::genRootNote, 1 }, "PCM Root Note", 0, 127, snesText::kDefaultRootNote,
        AudioParameterIntAttributes()
            .withStringFromValueFunction([](int note, int) { return juce::MidiMessage::getMidiNoteName(note, true, true, 4); })
            .withValueFromStringFunction([](const juce::String& typed) { return snesText::noteFromText(typed); })));
    layout.add(std::make_unique<AudioParameterBool>(ParameterID { ids::genLoop, 1 }, "PCM Loop", false));

    // The SN76489. Its rates are the chip's three fixed ones, then the one that follows the note.
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ids::psgChannel, 1 }, "PSG Channel",
                                                      juce::StringArray { "Tone", "Noise" }, 0));
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ids::psgNoiseType, 1 }, "PSG Noise Type",
                                                      juce::StringArray { "White", "Periodic" }, 0));
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ids::psgNoiseRate, 1 }, "PSG Noise Rate",
                                                      juce::StringArray { "High", "Mid", "Low", "Note" }, 3));
    layout.add(time(ids::psgAttack, "PSG Attack", 2000.0f, 0.0f));
    layout.add(time(ids::psgDecay, "PSG Decay", 2000.0f, 0.0f));
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID { ids::psgSustain, 1 }, "PSG Sustain",
                                                     NormalisableRange<float>(0.0f, 15.0f, 1.0f), 15.0f));
    layout.add(time(ids::psgRelease, "PSG Release", 2000.0f, 50.0f));

    // The NES's imports and cartridge chips. The envelope's knobs are the 2A03's, shared by the three chips.
    layout.add(std::make_unique<AudioParameterBool>(ParameterID { ids::nesUseInstrument, 1 }, "FTI Instrument", false));
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ids::vrc6Channel, 1 }, "VRC6 Channel",
                                                      juce::StringArray { "Pulse", "Saw" }, 0));
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ids::vrc6Duty, 1 }, "VRC6 Duty",
                                                      juce::StringArray { "6%", "13%", "19%", "25%", "31%", "38%", "44%", "50%" }, 7));
    juce::StringArray waveNames;
    for (const auto* name : kN163WaveNames)
        waveNames.add(name);
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { ids::n163Wave, 1 }, "N163 Wave", waveNames, 0));
    layout.add(std::make_unique<AudioParameterInt>(ParameterID { ids::n163Channels, 1 }, "N163 Channels", 1, 8, 1));
    layout.add(std::make_unique<AudioParameterInt>(
        ParameterID { ids::dpcmRootNote, 1 }, "DPCM Root Note", 0, 127, snesText::kDefaultRootNote,
        AudioParameterIntAttributes()
            .withStringFromValueFunction([](int note, int) { return juce::MidiMessage::getMidiNoteName(note, true, true, 4); })
            .withValueFromStringFunction([](const juce::String& typed) { return snesText::noteFromText(typed); })));
    layout.add(std::make_unique<AudioParameterBool>(ParameterID { ids::dpcmLoop, 1 }, "DPCM Loop", false));
    return layout;
}
} // namespace retrochip
