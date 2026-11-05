// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

// Loads the built plugin as a host does, through its VST3 and, on macOS, its Audio Unit interface: not the plugin's
// own classes, but the bundles a DAW finds in its plug-in folder. Live, Reaper, Bitwig, Cubase and FL Studio load
// the VST3 this way; Logic and GarageBand the Audio Unit.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <cmath>
#include <cstdio>
#include <memory>

namespace
{
int failures = 0;

#define CHECK(condition)                                                                       \
    do                                                                                         \
    {                                                                                          \
        if (!(condition))                                                                      \
        {                                                                                      \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);               \
            ++failures;                                                                        \
        }                                                                                      \
    } while (false)

constexpr int kNumPrograms = 72;
constexpr int kProgramsPerChip = 12;
constexpr int kEditorWidth = 760;
constexpr int kEditorHeight = 520;

juce::AudioPluginFormat* formatNamed(juce::AudioPluginFormatManager& manager, const juce::String& name)
{
    for (auto* format : manager.getFormats())
        if (format->getName() == name)
            return format;
    return nullptr;
}

std::unique_ptr<juce::AudioPluginInstance> load(juce::AudioPluginFormatManager& manager, const juce::String& formatName,
                                                const juce::String& path, double rate, int block)
{
    auto* format = formatNamed(manager, formatName);
    CHECK(format != nullptr);
    if (format == nullptr)
        return nullptr;

    juce::OwnedArray<juce::PluginDescription> types;
    format->findAllTypesForFile(types, path);
    CHECK(types.size() == 1);
    if (types.isEmpty())
    {
        std::printf("  FAILED: %s finds no plugin in %s\n", formatName.toRawUTF8(), path.toRawUTF8());
        return nullptr;
    }

    const auto& description = *types.getFirst();
    CHECK(description.name == "RetroChip");
    CHECK(description.manufacturerName == "SleeperMusic");
    CHECK(description.isInstrument);

    juce::String error;
    auto plugin = manager.createPluginInstance(description, rate, block, error);
    if (plugin == nullptr)
        std::printf("  FAILED: %s does not load: %s\n", formatName.toRawUTF8(), error.toRawUTF8());
    CHECK(plugin != nullptr);
    return plugin;
}

juce::AudioProcessorParameter* parameterNamed(juce::AudioPluginInstance& plugin, const juce::String& name)
{
    for (auto* parameter : plugin.getParameters())
        if (parameter->getName(64) == name)
            return parameter;
    return nullptr;
}

struct Render
{
    float peak = 0.0f;
    bool finite = true;
};

// Plays `seconds` of audio in blocks of `block`, with `midi` at the start of the first block.
Render render(juce::AudioPluginInstance& plugin, int block, double rate, double seconds, const juce::MidiBuffer& midi)
{
    Render result;
    juce::AudioBuffer<float> buffer(juce::jmax(2, plugin.getTotalNumOutputChannels()), block);
    const auto blocks = static_cast<int>(std::ceil(seconds * rate / block));
    for (int b = 0; b < blocks; ++b)
    {
        buffer.clear();
        juce::MidiBuffer events;
        if (b == 0)
            events = midi;
        plugin.processBlock(buffer, events);
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (int i = 0; i < block; ++i)
            {
                const float sample = buffer.getSample(channel, i);
                result.finite = result.finite && std::isfinite(sample);
                result.peak = std::max(result.peak, std::abs(sample));
            }
    }
    return result;
}

juce::MidiBuffer noteOn(int note)
{
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)), 0);
    return midi;
}

juce::MidiBuffer noteOff(int note)
{
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);
    return midi;
}

void checkLayout(juce::AudioPluginInstance& plugin)
{
    CHECK(plugin.getTotalNumInputChannels() == 0);
    CHECK(plugin.getTotalNumOutputChannels() == 2);
    CHECK(plugin.acceptsMidi());
    CHECK(plugin.getName() == "RetroChip");

    for (const char* name : { "Console", "Attack", "Release", "Gain", "Timbre", "Instrument", "Channel" })
        if (parameterNamed(plugin, name) == nullptr)
        {
            std::printf("  FAILED: no parameter %s\n", name);
            ++failures;
        }
}

void checkPrograms(juce::AudioPluginInstance& plugin)
{
    CHECK(plugin.getNumPrograms() == kNumPrograms);
    for (int i = 0; i < plugin.getNumPrograms(); ++i)
        CHECK(plugin.getProgramName(i).isNotEmpty());
    plugin.setCurrentProgram(30);
    CHECK(plugin.getCurrentProgram() == 30);
}

// The first preset of each chip sounds, at the host's rates and block sizes, odd ones too, and stays finite.
void checkSound(juce::AudioPluginInstance& plugin)
{
    for (const double rate : { 44100.0, 48000.0, 96000.0 })
        for (const int block : { 64, 441, 512 })
        {
            plugin.releaseResources();
            plugin.prepareToPlay(rate, block);
            for (int chip = 0; chip < kNumPrograms / kProgramsPerChip; ++chip)
            {
                const int program = chip * kProgramsPerChip;
                plugin.setCurrentProgram(program);
                render(plugin, block, rate, 0.05, {}); // the switch settles
                const auto sounding = render(plugin, block, rate, 0.3, noteOn(60));
                const auto released = render(plugin, block, rate, 0.5, noteOff(60));
                if (sounding.peak < 0.01f)
                    std::printf("  FAILED: program %d is silent at %.0f Hz, blocks of %d\n", program, rate, block);
                CHECK(sounding.peak >= 0.01f);
                CHECK(sounding.peak < 4.0f);
                CHECK(sounding.finite && released.finite);
            }
        }
}

// What a project saves comes back in a new instance: the program and a parameter changed by the host.
void checkState(juce::AudioPluginFormatManager& manager, const juce::String& formatName, const juce::String& path,
                juce::AudioPluginInstance& plugin)
{
    plugin.setCurrentProgram(40);
    auto* gain = parameterNamed(plugin, "Gain");
    CHECK(gain != nullptr);
    if (gain == nullptr)
        return;
    gain->setValueNotifyingHost(0.25f);
    const float saved = gain->getValue();

    juce::MemoryBlock state;
    plugin.getStateInformation(state);
    CHECK(state.getSize() > 0);

    auto restored = load(manager, formatName, path, 48000.0, 512);
    if (restored == nullptr)
        return;
    restored->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(restored->getCurrentProgram() == 40);
    if (auto* restoredGain = parameterNamed(*restored, "Gain"))
        CHECK(std::abs(restoredGain->getValue() - saved) < 0.001f);
    else
        CHECK(false);
}

void checkEditor(juce::AudioPluginInstance& plugin)
{
    CHECK(plugin.hasEditor());
    std::unique_ptr<juce::AudioProcessorEditor> editor(plugin.createEditorAndMakeActive());
    CHECK(editor != nullptr);
    if (editor == nullptr)
        return;
    CHECK(editor->getWidth() == kEditorWidth);
    CHECK(editor->getHeight() == kEditorHeight);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    editor.reset();
}

// Two instances at once, as two tracks of a project.
void checkTwoInstances(juce::AudioPluginFormatManager& manager, const juce::String& formatName, const juce::String& path)
{
    auto first = load(manager, formatName, path, 48000.0, 256);
    auto second = load(manager, formatName, path, 48000.0, 256);
    if (first == nullptr || second == nullptr)
        return;
    first->prepareToPlay(48000.0, 256);
    second->prepareToPlay(48000.0, 256);
    first->setCurrentProgram(0);
    second->setCurrentProgram(kProgramsPerChip);
    const auto a = render(*first, 256, 48000.0, 0.2, noteOn(64));
    const auto b = render(*second, 256, 48000.0, 0.2, noteOn(48));
    CHECK(a.peak > 0.01f && b.peak > 0.01f);
    CHECK(first->getCurrentProgram() == 0 && second->getCurrentProgram() == kProgramsPerChip);
}

void checkFormat(juce::AudioPluginFormatManager& manager, const juce::String& formatName, const juce::String& path)
{
    std::printf("%s: %s\n", formatName.toRawUTF8(), path.toRawUTF8());
    auto plugin = load(manager, formatName, path, 48000.0, 512);
    if (plugin == nullptr)
        return;
    checkLayout(*plugin);
    checkPrograms(*plugin);
    checkSound(*plugin);
    checkState(manager, formatName, path, *plugin);
    checkEditor(*plugin);
    checkTwoInstances(manager, formatName, path);
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::AudioPluginFormatManager manager;
    juce::addDefaultFormatsToManager(manager);

    checkFormat(manager, "VST3", RETROCHIP_VST3_PATH);
#if JUCE_MAC
    // The Audio Unit is found by its codes among those the system has registered: the installed copy, which the
    // build has just replaced.
    checkFormat(manager, "AudioUnit", "AudioUnit:Synths/aumu,Rc2a,Slpr");
#endif

    std::printf("host tests: %d failed checks\n", failures);
    return failures == 0 ? 0 : 1;
}
