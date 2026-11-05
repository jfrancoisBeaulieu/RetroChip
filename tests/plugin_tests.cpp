// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "PluginEditor.h"
#include "Localisation.h"
#include "PluginProcessor.h"
#include "Parameters.h"
#include "Presets.h"
#include "SampleImporter.h"
#include "Sf2Builder.h"
#include "SoundFontLibrary.h"
#include "SoundFontLoader.h"
#include "genesis/FmPatches.h"
#include "genesis/GenesisEngine.h"
#include "genesis/PsgEngine.h"
#include "nes/Dpcm.h"
#include "nes/N163Waves.h"
#include "snes/SnesBank.h"
#include "snes/SnesEngine.h"
#include "soundfont/SoundFontBank.h"
#include "soundfont/SoundFontEngine.h"

#include <atomic>
#include <set>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#if !JUCE_WINDOWS
 #include <sys/stat.h>
 #include <unistd.h>
#endif

namespace
{
int failures = 0;

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                      \
        }                                                                    \
    } while (false)

using retrochip::RetroChipProcessor;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 512;

void setParam(RetroChipProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.apvts.getParameter(id);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

float getParam(RetroChipProcessor& processor, const char* id)
{
    return processor.apvts.getRawParameterValue(id)->load();
}

// Runs blocks with an optional note at the start; returns the peak and checks for NaN.
double run(RetroChipProcessor& processor, int blocks, int note = -1)
{
    juce::AudioBuffer<float> buffer(2, kBlock);
    double peak = 0.0;
    for (int i = 0; i < blocks; ++i)
    {
        juce::MidiBuffer midi;
        if (i == 0 && note >= 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(120)), 10);
        processor.processBlock(buffer, midi);
        for (int channel = 0; channel < 2; ++channel)
        {
            const auto* data = buffer.getReadPointer(channel);
            for (int s = 0; s < kBlock; ++s)
            {
                CHECK(std::isfinite(data[s]));
                peak = std::max(peak, std::abs(static_cast<double>(data[s])));
            }
        }
    }
    return peak;
}

void allOff(RetroChipProcessor& processor)
{
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
    processor.processBlock(buffer, midi);
}

juce::Component* findById(juce::Component& root, const juce::String& id)
{
    for (auto* child : root.getChildren())
    {
        if (child->getComponentID() == id)
            return child;
        if (auto* found = findById(*child, id))
            return found;
    }
    return nullptr;
}

void checkEnabled(juce::Component& editor, const char* id, bool expected)
{
    auto* component = findById(editor, id);
    CHECK(component != nullptr);
    if (component != nullptr && component->isEnabled() != expected)
    {
        std::printf("  FAILED: %s enabled should be %d\n", id, expected ? 1 : 0);
        ++failures;
    }
}

void checkTitles(juce::Component& root)
{
    for (auto* child : root.getChildren())
    {
        if (!child->isVisible())
            continue;

        // The +/- buttons inside a slider are not exposed; the slider carries the name.
        if (!child->isAccessible())
            continue;

        if (child->getWantsKeyboardFocus() || dynamic_cast<juce::Button*>(child) != nullptr
            || dynamic_cast<juce::Slider*>(child) != nullptr || dynamic_cast<juce::ComboBox*>(child) != nullptr)
        {
            if (child->getTitle().isEmpty() && dynamic_cast<juce::Label*>(child) == nullptr)
            {
                std::printf("  FAILED: untitled control %s\n", child->getComponentID().toRawUTF8());
                ++failures;
            }
        }
        checkTitles(*child);
    }
}

// What the file system can do to a test's files. Windows has neither these permissions nor symbolic links an
// ordinary user may make, nor sparse files of a given size, so there the tests leave those cases out.
#if JUCE_WINDOWS
constexpr bool kPosixFiles = false;
#else
constexpr bool kPosixFiles = true;
#endif

bool setMode(const juce::File& file, int mode)
{
#if JUCE_WINDOWS
    juce::ignoreUnused(file, mode);
    return false;
#else
    return ::chmod(file.getFullPathName().toRawUTF8(), static_cast<mode_t>(mode)) == 0;
#endif
}

bool setSparseSize(const juce::File& file, juce::int64 size)
{
#if JUCE_WINDOWS
    juce::ignoreUnused(file, size);
    return false;
#else
    return ::truncate(file.getFullPathName().toRawUTF8(), static_cast<off_t>(size)) == 0;
#endif
}

void saveSnapshot(juce::Component& editor, const char* fileName)
{
    const auto image = editor.createComponentSnapshot(editor.getLocalBounds(), true, 2.0f);
    const auto file = juce::File(RETROCHIP_SNAPSHOT_DIR).getChildFile(fileName); // the build folder, wherever the test runs from
    file.deleteFile();
    juce::FileOutputStream stream(file);
    juce::PNGImageFormat format;
    CHECK(stream.openedOk() && format.writeImageToStream(image, stream));
}

// Renders a program the way the reference files were made: note 60 at velocity 120, note-off at 24000,
// 48000 samples in total. Returns the stereo output interleaved as L, R, L, R...
std::vector<float> renderReference(RetroChipProcessor& processor, int program)
{
    constexpr int kNoteOffAt = 24000;
    constexpr int kTotal = 48000;

    processor.prepareToPlay(kSampleRate, kBlock);
    processor.setCurrentProgram(program);

    std::vector<float> result;
    result.reserve(static_cast<size_t>(kTotal) * 2);
    juce::AudioBuffer<float> buffer(2, kBlock);
    for (int start = 0; start < kTotal; start += kBlock)
    {
        const int count = std::min(kBlock, kTotal - start);
        juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), 2, count);
        juce::MidiBuffer midi;
        if (start == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(120)), 0);
        if (kNoteOffAt >= start && kNoteOffAt < start + count)
            midi.addEvent(juce::MidiMessage::noteOff(1, 60), kNoteOffAt - start);
        processor.processBlock(view, midi);
        for (int s = 0; s < count; ++s)
        {
            result.push_back(view.getSample(0, s));
            result.push_back(view.getSample(1, s));
        }
    }
    return result;
}

juce::File referenceFile(const char* name)
{
    return juce::File(RETROCHIP_REFERENCE_DIR).getChildFile(name);
}

constexpr int kReferencePrograms[] = { 0, 6, 9 };

juce::String referenceName(int program)
{
    return "program_" + juce::String(program) + ".f32";
}

// Run once on the unmodified 2A03 code to record what later changes must still produce.
int writeReferences()
{
    const auto directory = juce::File(RETROCHIP_REFERENCE_DIR);
    directory.createDirectory();

    for (const int program : kReferencePrograms)
    {
        RetroChipProcessor processor;
        const auto samples = renderReference(processor, program);
        const auto file = directory.getChildFile(referenceName(program));
        file.deleteFile();
        juce::FileOutputStream stream(file);
        if (!stream.openedOk() || !stream.write(samples.data(), samples.size() * sizeof(float)))
        {
            std::printf("could not write %s\n", file.getFullPathName().toRawUTF8());
            return 1;
        }
        std::printf("wrote %s\n", file.getFullPathName().toRawUTF8());
    }

    RetroChipProcessor processor;
    processor.setCurrentProgram(4);
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    const auto file = directory.getChildFile("nes_state.bin");
    if (!file.replaceWithData(state.getData(), state.getSize()))
    {
        std::printf("could not write %s\n", file.getFullPathName().toRawUTF8());
        return 1;
    }
    std::printf("wrote %s\n", file.getFullPathName().toRawUTF8());
    return 0;
}

constexpr const char* kConsoleId = retrochip::ids::console;

bool setConsole(RetroChipProcessor& processor, int index)
{
    auto* parameter = processor.apvts.getParameter(kConsoleId);
    CHECK(parameter != nullptr);
    if (parameter == nullptr)
        return false;
    parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(index)));
    return true;
}

float consoleValue(RetroChipProcessor& processor)
{
    auto* value = processor.apvts.getRawParameterValue(kConsoleId);
    CHECK(value != nullptr);
    return value != nullptr ? value->load() : -1.0f;
}

juce::MemoryBlock loadReference(const juce::String& name)
{
    juce::MemoryBlock data;
    CHECK(referenceFile(name.toRawUTF8()).loadFileAsData(data));
    return data;
}

void nes_render_matches_references()
{
    for (const int program : kReferencePrograms)
    {
        const auto name = referenceName(program);
        const auto reference = loadReference(name);
        RetroChipProcessor processor;
        const auto rendered = renderReference(processor, program);

        const size_t count = rendered.size();
        CHECK(reference.getSize() == count * sizeof(float));
        if (reference.getSize() != count * sizeof(float))
            continue;

        const auto* expected = static_cast<const float*>(reference.getData());
        double maxDifference = 0.0;
        for (size_t i = 0; i < count; ++i)
            maxDifference = std::max(maxDifference, std::abs(static_cast<double>(rendered[i]) - static_cast<double>(expected[i])));
        if (!(maxDifference < 1e-5))
        {
            std::printf("  FAILED: program %d differs from %s by %g\n", program, name.toRawUTF8(), maxDifference);
            ++failures;
        }
    }
}

void nes_state_loads_as_2a03()
{
    const auto state = loadReference("nes_state.bin");
    RetroChipProcessor processor;
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

    CHECK(consoleValue(processor) == 0.0f);
    CHECK(processor.getCurrentProgram() == 4);
    CHECK(std::abs(getParam(processor, retrochip::ids::attack) - 400.0f) < 1.0f);
    CHECK(std::abs(getParam(processor, retrochip::ids::decay) - 300.0f) < 1.0f);
    CHECK(std::abs(getParam(processor, retrochip::ids::sustain) - 9.0f) < 0.1f);
    CHECK(std::abs(getParam(processor, retrochip::ids::release) - 500.0f) < 1.0f);
    CHECK(std::abs(getParam(processor, retrochip::ids::vibratoRate) - 4.5f) < 0.1f);
    CHECK(std::abs(getParam(processor, retrochip::ids::vibratoDepth) - 10.0f) < 0.5f);
    CHECK(getParam(processor, retrochip::ids::channel) == 0.0f);
    CHECK(getParam(processor, retrochip::ids::duty) == 2.0f);
}

void old_parameter_ids_are_unchanged()
{
    RetroChipProcessor processor;
    for (const char* id : { "channel", "duty", "noiseMode", "attack", "decay", "sustain", "release",
                            "vibratoRate", "vibratoDepth", "glide", "velocity", "gain" })
    {
        if (processor.apvts.getParameter(id) == nullptr)
        {
            std::printf("  FAILED: parameter %s is missing\n", id);
            ++failures;
        }
    }

    const auto state = loadReference("nes_state.bin");
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(std::abs(getParam(processor, "attack") - 400.0f) < 1.0f);
}

void console_change_stops_held_notes()
{
    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(run(processor, 8, 69) > 0.02);

    // The 2A03 note is still held when the console changes. Its engine renders nothing from here on.
    if (!setConsole(processor, 1))
        return;
    CHECK(run(processor, 4) == 0.0);

    // A note played on the other console must not reach the 2A03 either.
    run(processor, 2, 72);
    if (!setConsole(processor, 0))
        return;
    run(processor, 40);
    CHECK(run(processor, 4) < 1e-3);

    // The 2A03 plays again afterwards.
    CHECK(run(processor, 8, 69) > 0.02);
}

// Sends the messages at sample 0, runs the blocks and returns channel 0.
std::vector<float> processWith(RetroChipProcessor& processor, int blocks, const juce::MidiBuffer& first = {})
{
    std::vector<float> samples;
    juce::AudioBuffer<float> buffer(2, kBlock);
    for (int i = 0; i < blocks; ++i)
    {
        juce::MidiBuffer midi;
        if (i == 0)
            midi = first;
        processor.processBlock(buffer, midi);
        samples.insert(samples.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + kBlock);
    }
    return samples;
}

// A note played after the round trip, with its note-off, for 40 blocks.
std::vector<float> playProbeNote(RetroChipProcessor& processor)
{
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(120)), 0);
    auto samples = processWith(processor, 10, on);
    juce::MidiBuffer off;
    off.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
    const auto tail = processWith(processor, 40, off);
    samples.insert(samples.end(), tail.begin(), tail.end());
    return samples;
}

void console_change_clears_sustain_and_pitch_bend()
{
    // The 2A03 renders 4 blocks while the note is held and 40 after coming back. The envelope ticks run on
    // the engine's own sample count, so the reference renders the same 44 blocks (silent) before the probe.
    const auto reference = [&]
    {
        RetroChipProcessor fresh;
        fresh.prepareToPlay(kSampleRate, kBlock);
        processWith(fresh, 44);
        return playProbeNote(fresh);
    }();

    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);

    // Pedal down, bend up and a held note on the 2A03.
    juce::MidiBuffer held;
    held.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    held.addEvent(juce::MidiMessage::pitchWheel(1, 16383), 0);
    held.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(120)), 1);
    processWith(processor, 4, held);

    // Leave, then lift the pedal and recentre the wheel while away. These go to the other console.
    if (!setConsole(processor, 1))
        return;
    processWith(processor, 2);
    juce::MidiBuffer lift;
    lift.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
    lift.addEvent(juce::MidiMessage::pitchWheel(1, 8192), 0);
    processWith(processor, 2, lift);

    if (!setConsole(processor, 0))
        return;
    processWith(processor, 40);

    // The probe note must sound and end like one on a fresh processor: same pitch, and no hanging note.
    const auto probe = playProbeNote(processor);
    CHECK(probe.size() == reference.size());
    if (probe.size() != reference.size())
        return;

    double largest = 0.0;
    for (size_t i = 0; i < probe.size(); ++i)
        largest = std::max(largest, std::abs(static_cast<double>(probe[i]) - static_cast<double>(reference[i])));
    if (!(largest < 1e-5))
    {
        std::printf("  FAILED: note after a console round trip differs from a fresh one by %g\n", largest);
        ++failures;
    }

    double tailPeak = 0.0;
    for (size_t i = probe.size() - 4 * kBlock; i < probe.size(); ++i)
        tailPeak = std::max(tailPeak, std::abs(static_cast<double>(probe[i])));
    CHECK(tailPeak < 1e-3);
}

void console_selection_picks_engine_and_latency()
{
    using retrochip::Console;
    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(processor.getActiveConsole() == Console::Chip2A03);
    CHECK(processor.engineFor(Console::Chip2A03) != nullptr);
    CHECK(processor.engineFor(Console::Ym2612) != nullptr);
    CHECK(processor.engineFor(Console::SDsp) != nullptr);
    CHECK(processor.getLatencySamples() == retrochip::BandLimitedBuffer::kLatencySamples);

    // The latency is reported from the message thread, so let it run before checking.
    const auto flushMessages = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil(20); };

    setConsole(processor, 2);
    CHECK(processor.getActiveConsole() == Console::SDsp);
    run(processor, 1);
    flushMessages();
    auto* snes = dynamic_cast<retrochip::SnesEngine*>(processor.engineFor(Console::SDsp));
    CHECK(snes != nullptr && snes->latencySamples() > 0);
    CHECK(snes != nullptr && processor.getLatencySamples() == snes->latencySamples());

    setConsole(processor, 0);
    run(processor, 1);
    flushMessages();
    CHECK(processor.getLatencySamples() == retrochip::BandLimitedBuffer::kLatencySamples);

    // A 2A03 preset brings the 2A03 back.
    setConsole(processor, 1);
    processor.setCurrentProgram(3);
    CHECK(processor.getActiveConsole() == Console::Chip2A03);
}

void console_selector_shows_chip_names()
{
    RetroChipProcessor processor;
    auto* parameter = dynamic_cast<juce::AudioParameterChoice*>(processor.apvts.getParameter(kConsoleId));
    CHECK(parameter != nullptr);
    if (parameter == nullptr)
        return;
    CHECK(parameter->choices == juce::StringArray({ "NES", "Genesis FM", "SNES", "SNES SoundFont", "Genesis PSG", "NES VRC6", "NES N163" }));
    CHECK(parameter->getIndex() == 0);
}

void midi_note_produces_sound()
{
    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(run(processor, 2) == 0.0);
    CHECK(run(processor, 8, 69) > 0.02);
}

void note_off_ends_sound()
{
    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    run(processor, 8, 69);
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
    processor.processBlock(buffer, midi);
    run(processor, 40);
    CHECK(run(processor, 4) < 1e-3);
}

void pitch_wheel_and_sustain_reach_the_engine()
{
    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    setParam(processor, retrochip::ids::release, 0.0f);
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    midi.addEvent(juce::MidiMessage::pitchWheel(1, 16383), 0);
    midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(120)), 1);
    midi.addEvent(juce::MidiMessage::noteOff(1, 69), 2);
    processor.processBlock(buffer, midi);
    CHECK(run(processor, 20) > 0.02); // pedal keeps it sounding after the note-off

    juce::MidiBuffer lift;
    lift.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
    processor.processBlock(buffer, lift);
    run(processor, 40);
    CHECK(run(processor, 4) < 1e-3);
}

void there_are_twelve_2a03_presets_that_all_sound()
{
    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(processor.getNumPrograms() == 72); // 12 per chip
    for (int i = 0; i < 12; ++i)
    {
        processor.setCurrentProgram(i);
        CHECK(processor.getCurrentProgram() == i);
        CHECK(processor.getActiveConsole() == retrochip::Console::Chip2A03);
        CHECK(processor.getProgramName(i).isNotEmpty());
        CHECK(!processor.getProgramName(i).containsIgnoreCase("nes"));
        CHECK(!processor.getProgramName(i).containsIgnoreCase("nintendo"));
        allOff(processor);
        if (run(processor, 40, 60) <= 0.005)
        {
            std::printf("  FAILED: preset %s is silent\n", processor.getProgramName(i).toRawUTF8());
            ++failures;
        }
        allOff(processor);
    }
}

void state_round_trip_keeps_values_and_program()
{
    juce::MemoryBlock saved;
    {
        RetroChipProcessor processor;
        processor.setCurrentProgram(4);
        setParam(processor, retrochip::ids::attack, 321.0f);
        setParam(processor, retrochip::ids::channel, 2.0f);
        processor.getStateInformation(saved);
    }
    RetroChipProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    CHECK(std::abs(getParam(restored, retrochip::ids::attack) - 321.0f) < 1.0f);
    CHECK(getParam(restored, retrochip::ids::channel) == 2.0f);
    CHECK(restored.getCurrentProgram() == 4);
}

void corrupt_state_keeps_current_values()
{
    RetroChipProcessor processor;
    setParam(processor, retrochip::ids::attack, 500.0f);
    const char garbage[] = "this is not a saved state";
    processor.setStateInformation(garbage, sizeof(garbage));
    processor.setStateInformation(nullptr, 0);
    CHECK(std::abs(getParam(processor, retrochip::ids::attack) - 500.0f) < 1.0f);
}

void partial_state_uses_defaults_for_missing_parameters()
{
    RetroChipProcessor source;
    setParam(source, retrochip::ids::attack, 321.0f);
    setParam(source, retrochip::ids::release, 900.0f);
    auto state = source.apvts.copyState();
    state.removeChild(state.getChildWithProperty("id", retrochip::ids::release), nullptr);

    juce::MemoryBlock saved;
    juce::AudioProcessor::copyXmlToBinary(*state.createXml(), saved);

    RetroChipProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    CHECK(std::abs(getParam(restored, retrochip::ids::attack) - 321.0f) < 1.0f);
    CHECK(std::abs(getParam(restored, retrochip::ids::release) - 50.0f) < 1.0f);
}

void preset_change_with_held_notes_is_safe()
{
    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    run(processor, 4, 60);
    for (int i = 0; i < processor.getNumPrograms(); ++i)
    {
        processor.setCurrentProgram(i);
        run(processor, 4);
    }
    allOff(processor);
    processor.setCurrentProgram(0);
    run(processor, 40);
    CHECK(run(processor, 4) < 1e-3);
}

void empty_and_large_blocks_are_safe()
{
    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> empty(2, 0);
    processor.processBlock(empty, midi);

    juce::AudioBuffer<float> large(2, 20000);
    midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(120)), 19000);
    processor.processBlock(large, midi);
    CHECK(large.getMagnitude(0, 19000) == 0.0f);
    CHECK(large.getMagnitude(19100, 900) > 0.01f);
}

void sample_rate_change_is_safe()
{
    RetroChipProcessor processor;
    processor.prepareToPlay(44100.0, 256);
    run(processor, 4, 60);
    processor.prepareToPlay(96000.0, 1024);
    CHECK(run(processor, 4, 60) > 0.01);
}

void gain_scales_output()
{
    const auto levelAt = [](float gainDb)
    {
        RetroChipProcessor processor;
        setParam(processor, retrochip::ids::gain, gainDb);
        processor.prepareToPlay(kSampleRate, kBlock);
        run(processor, 2);
        return run(processor, 8, 69);
    };
    const double ratio = levelAt(-12.0f) / levelAt(0.0f);
    CHECK(ratio > 0.2 && ratio < 0.3);
}

void reports_latency_and_formats()
{
    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(processor.getLatencySamples() == retrochip::BandLimitedBuffer::kLatencySamples);
    CHECK(processor.acceptsMidi());
    CHECK(!processor.producesMidi());
    CHECK(processor.getName() == "RetroChip");
    CHECK(processor.getTailLengthSeconds() == 6.0); // the echo rings on after the longest release
    juce::AudioProcessor::BusesLayout mono, stereo, surround;
    mono.outputBuses.add(juce::AudioChannelSet::mono());
    stereo.outputBuses.add(juce::AudioChannelSet::stereo());
    surround.outputBuses.add(juce::AudioChannelSet::create5point1());
    CHECK(processor.isBusesLayoutSupported(mono));
    CHECK(processor.isBusesLayoutSupported(stereo));
    CHECK(!processor.isBusesLayoutSupported(surround));
}

void editor_has_fixed_size_and_titles()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    CHECK(processor.hasEditor());
    CHECK(editor->getWidth() == 760);
    CHECK(editor->getHeight() == 520);
    checkTitles(*editor);
    saveSnapshot(*editor, "editor_pulse.png");
}

void editor_greys_out_controls_by_channel()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());

    setParam(processor, retrochip::ids::channel, 0.0f);
    checkEnabled(*editor, retrochip::ids::duty, true);
    checkEnabled(*editor, retrochip::ids::noiseMode, false);
    checkEnabled(*editor, retrochip::ids::attack, true);
    checkEnabled(*editor, retrochip::ids::velocity, true);
    checkEnabled(*editor, retrochip::ids::vibratoDepth, true);

    setParam(processor, retrochip::ids::channel, 1.0f);
    checkEnabled(*editor, retrochip::ids::duty, false);
    checkEnabled(*editor, retrochip::ids::noiseMode, false);
    checkEnabled(*editor, retrochip::ids::attack, false);
    checkEnabled(*editor, retrochip::ids::release, false);
    checkEnabled(*editor, retrochip::ids::velocity, false);
    checkEnabled(*editor, retrochip::ids::glide, true);
    saveSnapshot(*editor, "editor_triangle.png");

    setParam(processor, retrochip::ids::channel, 2.0f);
    checkEnabled(*editor, retrochip::ids::duty, false);
    checkEnabled(*editor, retrochip::ids::noiseMode, true);
    checkEnabled(*editor, retrochip::ids::attack, true);
    checkEnabled(*editor, retrochip::ids::glide, false);
    checkEnabled(*editor, retrochip::ids::vibratoRate, false);
    saveSnapshot(*editor, "editor_noise.png");
}

void editor_shows_2a03_panels_only_on_2a03()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    const auto visible = [&](const char* id)
    {
        auto* component = findById(*editor, id);
        CHECK(component != nullptr);
        return component != nullptr && component->isVisible();
    };

    CHECK(visible("console"));
    CHECK(visible(retrochip::ids::channel));
    CHECK(visible(retrochip::ids::attack));
    CHECK(visible(retrochip::ids::gain));
    CHECK(visible(retrochip::ids::velocity));

    setParam(processor, kConsoleId, 1.0f);
    CHECK(visible("console"));
    CHECK(!visible(retrochip::ids::channel));
    CHECK(!visible(retrochip::ids::duty));
    CHECK(!visible(retrochip::ids::attack));
    CHECK(visible(retrochip::ids::gain)); // shared by all consoles
    CHECK(visible(retrochip::ids::velocity));

    setParam(processor, kConsoleId, 2.0f);
    CHECK(!visible(retrochip::ids::channel));

    setParam(processor, kConsoleId, 0.0f);
    CHECK(visible(retrochip::ids::channel));
    CHECK(visible(retrochip::ids::attack));

    auto* ym = dynamic_cast<juce::Button*>(findById(*editor, "console_1"));
    CHECK(ym != nullptr);
    if (ym != nullptr)
    {
        CHECK(ym->getButtonText() == "FM");
        ym->setToggleState(true, juce::sendNotificationSync);
        CHECK(getParam(processor, kConsoleId) == 1.0f);
        CHECK(!visible(retrochip::ids::channel));
    }
}

void editor_controls_change_parameters()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto* triangle = dynamic_cast<juce::Button*>(findById(*editor, "channel_1"));
    CHECK(triangle != nullptr);
    if (triangle != nullptr)
    {
        triangle->setToggleState(true, juce::sendNotificationSync);
        CHECK(getParam(processor, retrochip::ids::channel) == 1.0f);
    }

    auto* preset = dynamic_cast<juce::ComboBox*>(findById(*editor, "preset"));
    CHECK(preset != nullptr);
    if (preset != nullptr)
    {
        CHECK(preset->getNumItems() == 72); // section headings don't count
        preset->setSelectedItemIndex(11, juce::sendNotificationSync);
        CHECK(processor.getCurrentProgram() == 11);
        preset->setSelectedItemIndex(12, juce::sendNotificationSync);
        CHECK(processor.getCurrentProgram() == 12);
        CHECK(processor.getActiveConsole() == retrochip::Console::Ym2612);
        preset->setSelectedItemIndex(24, juce::sendNotificationSync);
        CHECK(processor.getCurrentProgram() == 24);
        CHECK(processor.getActiveConsole() == retrochip::Console::SDsp);
        preset->setSelectedItemIndex(35, juce::sendNotificationSync);
        CHECK(processor.getCurrentProgram() == 35);
        CHECK(processor.getProgramName(35) == "Slow Pad");
        preset->setSelectedItemIndex(47, juce::sendNotificationSync);
        CHECK(processor.getCurrentProgram() == 47);
        CHECK(processor.getActiveConsole() == retrochip::Console::Psg);
    }
}

// ---- YM2612 ----

using retrochip::Console;
using retrochip::GenesisEngine;
using retrochip::kFmPatches;

void selectYm2612(RetroChipProcessor& processor, int program = 12)
{
    processor.setCurrentProgram(program);
    CHECK(processor.getActiveConsole() == Console::Ym2612);
}

// Plays note 60 for `blocks` blocks and returns channel 0.
std::vector<float> renderYmNote(RetroChipProcessor& processor, int blocks = 12)
{
    processor.prepareToPlay(kSampleRate, kBlock);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 0);
    return processWith(processor, blocks, on);
}

double peakOf(const std::vector<float>& samples, size_t from = 0, size_t to = SIZE_MAX)
{
    double peak = 0.0;
    for (size_t i = from; i < std::min(to, samples.size()); ++i)
        peak = std::max(peak, std::abs(static_cast<double>(samples[i])));
    return peak;
}

double maxDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    CHECK(a.size() == b.size());
    double largest = 0.0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        largest = std::max(largest, std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    return largest;
}

void ym2612_note_produces_sound()
{
    RetroChipProcessor processor;
    selectYm2612(processor, 12);
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(run(processor, 2) < 1e-4); // idle chips are silent
    CHECK(run(processor, 8, 69) > 0.02);

    // The note ends, and nothing is left hanging.
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer off;
    off.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
    processor.processBlock(buffer, off);
    run(processor, 200);
    CHECK(run(processor, 4) < 1e-3);

    // Pitch bend, sustain and all-notes-off reach the chip engine.
    juce::MidiBuffer hold;
    hold.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    hold.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(120)), 1);
    hold.addEvent(juce::MidiMessage::noteOff(1, 60), 2);
    processor.processBlock(buffer, hold);
    CHECK(run(processor, 4) > 0.01); // the pedal keeps it sounding
    allOff(processor);
    run(processor, 200);
    CHECK(run(processor, 4) < 1e-3);

    // Gain is shared with the other consoles.
    const auto levelAt = [](float gainDb)
    {
        RetroChipProcessor p;
        selectYm2612(p);
        setParam(p, retrochip::ids::gain, gainDb);
        p.prepareToPlay(kSampleRate, kBlock);
        run(p, 2);
        return run(p, 8, 69);
    };
    const double ratio = levelAt(-12.0f) / levelAt(0.0f);
    CHECK(ratio > 0.2 && ratio < 0.3);
}

void ym2612_presets_sound()
{
    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    for (int i = 12; i < 24; ++i)
    {
        const auto& patch = kFmPatches[i - 12];
        processor.setCurrentProgram(i);
        CHECK(processor.getCurrentProgram() == i);
        CHECK(processor.getActiveConsole() == Console::Ym2612);
        CHECK(getParam(processor, retrochip::ids::genPatch) == static_cast<float>(i - 12));
        CHECK(getParam(processor, retrochip::ids::genAlgorithm) == static_cast<float>(patch.algorithm + 1));
        CHECK(getParam(processor, retrochip::ids::genFeedback) == static_cast<float>(patch.feedback));

        const auto name = processor.getProgramName(i);
        CHECK(name == juce::String(patch.name));
        for (const char* forbidden : { "nes", "nintendo", "genesis", "snes", "sega" })
            CHECK(!name.containsIgnoreCase(forbidden));

        allOff(processor);
        if (run(processor, 40, 60) <= 0.005)
        {
            std::printf("  FAILED: preset %s is silent\n", name.toRawUTF8());
            ++failures;
        }
        allOff(processor);
    }

    // Every program name is shown once.
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        CHECK(processor.getProgramName(i).isNotEmpty());

    // A 2A03 preset takes the console back and leaves a YM2612 note silent on it.
    processor.setCurrentProgram(0);
    CHECK(processor.getActiveConsole() == Console::Chip2A03);
}

void ym2612_preset_sets_shared_parameters()
{
    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    setParam(processor, retrochip::ids::vibratoDepth, 77.0f);
    setParam(processor, retrochip::ids::glide, 400.0f);
    for (int i = 12; i < 24; ++i)
    {
        processor.setCurrentProgram(i);
        const auto& preset = retrochip::kPresets[static_cast<size_t>(i)];
        CHECK(std::abs(getParam(processor, retrochip::ids::vibratoRate) - preset.vibratoRate) < 0.06f);
        CHECK(std::abs(getParam(processor, retrochip::ids::vibratoDepth) - preset.vibratoDepth) < 0.6f);
        CHECK(std::abs(getParam(processor, retrochip::ids::glide) - preset.glide) < 1.0f);
        CHECK(std::abs(getParam(processor, retrochip::ids::genBrightness) - preset.genBrightness) < 0.6f);
        CHECK(std::abs(getParam(processor, retrochip::ids::genAttack) - preset.genAttack) < 0.6f);
        CHECK(std::abs(getParam(processor, retrochip::ids::genDecay) - preset.genDecay) < 0.6f);
        CHECK(std::abs(getParam(processor, retrochip::ids::genRelease) - preset.genRelease) < 0.6f);
    }
}

void ym2612_parameters_reach_the_engine()
{
    const auto render = [](const char* id, float value, int program)
    {
        RetroChipProcessor processor;
        selectYm2612(processor, program);
        if (id != nullptr)
            setParam(processor, id, value);
        return renderYmNote(processor);
    };

    // FM Bass: every macro changes the sound.
    const auto base = render(nullptr, 0.0f, 13);
    CHECK(peakOf(base) > 0.02);
    CHECK(maxDifference(base, render(retrochip::ids::genAlgorithm, 8.0f, 13)) > 0.01);
    CHECK(maxDifference(base, render(retrochip::ids::genFeedback, 0.0f, 13)) > 0.005);
    CHECK(maxDifference(base, render(retrochip::ids::genBrightness, 100.0f, 13)) > 0.01);
    CHECK(maxDifference(base, render(retrochip::ids::genDecay, -100.0f, 13)) > 0.005);
    CHECK(maxDifference(base, render(retrochip::ids::vibratoDepth, 100.0f, 13)) > 0.0); // may start at zero phase
    CHECK(maxDifference(base, render(retrochip::ids::genPatch, 3.0f, 13)) > 0.01);

    // Positive attack means a longer one: Brass is quieter in its first 20 ms.
    const auto early = [&](float attack)
    {
        const auto samples = render(retrochip::ids::genAttack, attack, 17);
        return peakOf(samples, 0, 960);
    };
    CHECK(early(100.0f) < 0.5 * early(0.0f));

    // Release: a short one ends a released note sooner.
    const auto tailAfterOff = [](float release)
    {
        RetroChipProcessor processor;
        selectYm2612(processor, 13);
        setParam(processor, retrochip::ids::genRelease, release);
        processor.prepareToPlay(kSampleRate, kBlock);
        juce::MidiBuffer on;
        on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 0);
        processWith(processor, 4, on);
        juce::MidiBuffer off;
        off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        processWith(processor, 1, off);
        return peakOf(processWith(processor, 6));
    };
    CHECK(tailAfterOff(-100.0f) < 0.5 * tailAfterOff(100.0f));
}

void ym2612_plugin_drives_the_engine_with_the_parameters()
{
    // The plugin must give the engine what its parameters say, with the algorithm counted from 0.
    retrochip::GenesisParams params;
    params.patch = 5;
    params.algorithm = 2; // shown as 3
    params.feedback = 6;
    params.brightness = 0.4f;
    params.attack = -0.3f;
    params.decay = 0.25f;
    params.release = -0.6f;

    GenesisEngine engine;
    engine.setParams(params);
    engine.prepare(kSampleRate);
    engine.noteOn(60, 110);
    std::vector<float> direct(static_cast<size_t>(12 * kBlock));
    for (int block = 0; block < 12; ++block)
        engine.render(direct.data() + block * kBlock, kBlock);

    RetroChipProcessor processor;
    selectYm2612(processor, 12);
    setParam(processor, retrochip::ids::genPatch, 5.0f);
    setParam(processor, retrochip::ids::genAlgorithm, 3.0f);
    setParam(processor, retrochip::ids::genFeedback, 6.0f);
    setParam(processor, retrochip::ids::genBrightness, 40.0f);
    setParam(processor, retrochip::ids::genAttack, -30.0f);
    setParam(processor, retrochip::ids::genDecay, 25.0f);
    setParam(processor, retrochip::ids::genRelease, -60.0f);
    const auto viaPlugin = renderYmNote(processor);

    CHECK(peakOf(direct) > 0.02);
    CHECK(maxDifference(direct, viaPlugin) < 1e-4);

    // The same when the parameters change after prepareToPlay, as automation does while playing.
    RetroChipProcessor automated;
    automated.prepareToPlay(kSampleRate, kBlock);
    selectYm2612(automated, 12);
    processWith(automated, 2);
    setParam(automated, retrochip::ids::genPatch, 5.0f);
    setParam(automated, retrochip::ids::genAlgorithm, 3.0f);
    setParam(automated, retrochip::ids::genFeedback, 6.0f);
    setParam(automated, retrochip::ids::genBrightness, 40.0f);
    setParam(automated, retrochip::ids::genAttack, -30.0f);
    setParam(automated, retrochip::ids::genDecay, 25.0f);
    setParam(automated, retrochip::ids::genRelease, -60.0f);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 0);
    const auto viaAutomation = processWith(automated, 12, on);

    // The engine's 60 Hz clock ran through the two silent blocks, so the reference does the same.
    GenesisEngine later;
    later.prepare(kSampleRate);
    std::vector<float> silent(static_cast<size_t>(kBlock));
    later.render(silent.data(), kBlock);
    later.render(silent.data(), kBlock);
    later.setParams(params);
    later.noteOn(60, 110);
    std::vector<float> reference(static_cast<size_t>(12 * kBlock));
    for (int block = 0; block < 12; ++block)
        later.render(reference.data() + block * kBlock, kBlock);
    CHECK(maxDifference(reference, viaAutomation) < 1e-4);
}

void ym2612_parameters_do_not_reach_the_2a03()
{
    const auto render = [](float algorithm)
    {
        RetroChipProcessor processor;
        setParam(processor, retrochip::ids::genAlgorithm, algorithm);
        setParam(processor, retrochip::ids::genBrightness, 80.0f);
        processor.prepareToPlay(kSampleRate, kBlock);
        juce::MidiBuffer on;
        on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 0);
        return processWith(processor, 8, on);
    };
    CHECK(maxDifference(render(1.0f), render(8.0f)) == 0.0);
}

void ym2612_state_round_trip()
{
    juce::MemoryBlock saved;
    std::vector<float> expected;
    {
        RetroChipProcessor processor;
        processor.setCurrentProgram(16);
        setParam(processor, retrochip::ids::genPatch, 8.0f);
        setParam(processor, retrochip::ids::genAlgorithm, 3.0f);
        setParam(processor, retrochip::ids::genFeedback, 6.0f);
        setParam(processor, retrochip::ids::genBrightness, 40.0f);
        setParam(processor, retrochip::ids::genAttack, -30.0f);
        setParam(processor, retrochip::ids::genDecay, 25.0f);
        setParam(processor, retrochip::ids::genRelease, -60.0f);
        processor.getStateInformation(saved);
        expected = renderYmNote(processor);
    }

    RetroChipProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    CHECK(restored.getActiveConsole() == Console::Ym2612);
    CHECK(restored.getCurrentProgram() == 16);
    CHECK(getParam(restored, retrochip::ids::genPatch) == 8.0f);
    CHECK(getParam(restored, retrochip::ids::genAlgorithm) == 3.0f);
    CHECK(getParam(restored, retrochip::ids::genFeedback) == 6.0f);
    CHECK(std::abs(getParam(restored, retrochip::ids::genBrightness) - 40.0f) < 0.5f);
    CHECK(std::abs(getParam(restored, retrochip::ids::genAttack) + 30.0f) < 0.5f);
    CHECK(std::abs(getParam(restored, retrochip::ids::genDecay) - 25.0f) < 0.5f);
    CHECK(std::abs(getParam(restored, retrochip::ids::genRelease) + 60.0f) < 0.5f);

    // The restored plugin sounds like the saved one.
    const auto again = renderYmNote(restored);
    CHECK(peakOf(again) > 0.02);
    CHECK(maxDifference(expected, again) < 1e-6);
}

void ym2612_defaults_match_the_spec()
{
    RetroChipProcessor processor;
    CHECK(getParam(processor, retrochip::ids::genPatch) == 0.0f);
    CHECK(getParam(processor, retrochip::ids::genAlgorithm) == 5.0f);
    CHECK(getParam(processor, retrochip::ids::genFeedback) == 0.0f);
    for (const char* id : { retrochip::ids::genBrightness, retrochip::ids::genAttack, retrochip::ids::genDecay,
                            retrochip::ids::genRelease })
        CHECK(getParam(processor, id) == 0.0f);

    const auto range = [&](const char* id)
    {
        auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(processor.apvts.getParameter(id));
        CHECK(parameter != nullptr);
        return parameter != nullptr ? parameter->getNormalisableRange().getRange() : juce::Range<float>();
    };
    CHECK(range(retrochip::ids::genAlgorithm) == juce::Range<float>(1.0f, 8.0f));
    CHECK(range(retrochip::ids::genFeedback) == juce::Range<float>(0.0f, 7.0f));
    CHECK(range(retrochip::ids::genPatch) == juce::Range<float>(0.0f, 14.0f));
    for (const char* id : { retrochip::ids::genBrightness, retrochip::ids::genAttack, retrochip::ids::genDecay,
                            retrochip::ids::genRelease })
        CHECK(range(id) == juce::Range<float>(-100.0f, 100.0f));
}

void reported_latency_follows_console()
{
    RetroChipProcessor processor;
    const auto flushMessages = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil(20); };
    auto* genesis = dynamic_cast<GenesisEngine*>(processor.engineFor(Console::Ym2612));
    CHECK(genesis != nullptr);
    if (genesis == nullptr)
        return;

    for (const double rate : { 44100.0, 48000.0, 96000.0 })
    {
        setConsole(processor, 0);
        processor.prepareToPlay(rate, kBlock);
        CHECK(processor.getLatencySamples() == retrochip::BandLimitedBuffer::kLatencySamples);

        // Selected before prepareToPlay, the latency is the engine's at that rate.
        setConsole(processor, 1);
        processor.prepareToPlay(rate, kBlock);
        CHECK(genesis->latencySamples() > 0);
        CHECK(processor.getLatencySamples() == genesis->latencySamples());

        // Selected while running, it follows once the message thread has run.
        setConsole(processor, 0);
        run(processor, 1);
        flushMessages();
        CHECK(processor.getLatencySamples() == retrochip::BandLimitedBuffer::kLatencySamples);
        setConsole(processor, 1);
        run(processor, 1);
        flushMessages();
        CHECK(processor.getLatencySamples() == genesis->latencySamples());
    }

    // The latency depends on the rate.
    processor.prepareToPlay(44100.0, kBlock);
    const int at44 = processor.getLatencySamples();
    processor.prepareToPlay(96000.0, kBlock);
    CHECK(processor.getLatencySamples() != at44);

    // The S-DSP reports its own, which depends on the rate too.
    auto* snes = dynamic_cast<retrochip::SnesEngine*>(processor.engineFor(Console::SDsp));
    CHECK(snes != nullptr);
    if (snes == nullptr)
        return;
    for (const double rate : { 44100.0, 48000.0, 96000.0 })
    {
        setConsole(processor, 2);
        processor.prepareToPlay(rate, kBlock);
        CHECK(snes->latencySamples() > 0);
        CHECK(processor.getLatencySamples() == snes->latencySamples());
    }

    setConsole(processor, 0);
    run(processor, 1);
    flushMessages();
    setConsole(processor, 2);
    run(processor, 1);
    flushMessages();
    CHECK(processor.getLatencySamples() == snes->latencySamples());
}

void ym2612_timbre_selection_copies_algorithm_and_feedback()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    setParam(processor, kConsoleId, 1.0f);

    auto* timbre = dynamic_cast<juce::ComboBox*>(findById(*editor, retrochip::ids::genPatch));
    CHECK(timbre != nullptr);
    if (timbre == nullptr)
        return;
    CHECK(timbre->getNumItems() == 15);
    CHECK(timbre->getItemText(1) == "FM Bass");

    // Item 0 is the one showing, and picking the shown item again changes nothing, so it goes last.
    for (int n = 1; n <= 12; ++n)
    {
        const int i = n % 12;
        timbre->setSelectedItemIndex(i, juce::sendNotificationSync);
        CHECK(getParam(processor, retrochip::ids::genPatch) == static_cast<float>(i));
        CHECK(getParam(processor, retrochip::ids::genAlgorithm) == static_cast<float>(kFmPatches[i].algorithm + 1));
        CHECK(getParam(processor, retrochip::ids::genFeedback) == static_cast<float>(kFmPatches[i].feedback));
    }

    // The selector shows an automated change, and automation alone moves nothing else.
    setParam(processor, retrochip::ids::genAlgorithm, 2.0f);
    setParam(processor, retrochip::ids::genFeedback, 1.0f);
    setParam(processor, retrochip::ids::genPatch, 4.0f);
    CHECK(timbre->getSelectedItemIndex() == 4);
    CHECK(getParam(processor, retrochip::ids::genAlgorithm) == 2.0f);
    CHECK(getParam(processor, retrochip::ids::genFeedback) == 1.0f);

    // The algorithm buttons write the parameter, counting from 1.
    auto* eighth = dynamic_cast<juce::Button*>(findById(*editor, "genAlgorithm_7"));
    CHECK(eighth != nullptr);
    if (eighth != nullptr)
    {
        CHECK(eighth->getButtonText() == "8");
        eighth->setToggleState(true, juce::sendNotificationSync);
        CHECK(getParam(processor, retrochip::ids::genAlgorithm) == 8.0f);
    }
    auto* second = dynamic_cast<juce::Button*>(findById(*editor, "genAlgorithm_1"));
    CHECK(second != nullptr);
    if (second != nullptr)
    {
        second->setToggleState(true, juce::sendNotificationSync);
        CHECK(getParam(processor, retrochip::ids::genAlgorithm) == 2.0f);
    }
    setParam(processor, retrochip::ids::genAlgorithm, 6.0f);
    auto* sixth = dynamic_cast<juce::Button*>(findById(*editor, "genAlgorithm_5"));
    CHECK(sixth != nullptr && sixth->getToggleState());
}

void checkLayout(juce::Component& editor)
{
    std::vector<juce::Component*> shown;
    for (auto* child : editor.getChildren())
        if (child->isVisible())
            shown.push_back(child);

    for (size_t i = 0; i < shown.size(); ++i)
    {
        if (!editor.getLocalBounds().contains(shown[i]->getBounds()))
        {
            std::printf("  FAILED: %s is outside the editor\n", shown[i]->getComponentID().toRawUTF8());
            ++failures;
        }
        for (size_t j = i + 1; j < shown.size(); ++j)
        {
            // The gain knob's box spans its panel, with the velocity toggle drawn below the knob itself.
            const auto ids = shown[i]->getComponentID() + shown[j]->getComponentID();
            if (ids == "gainvelocity" || ids == "velocitygain")
                continue;
            if (shown[i]->getBounds().intersects(shown[j]->getBounds()))
            {
                std::printf("  FAILED: %s overlaps %s\n", shown[i]->getComponentID().toRawUTF8(),
                            shown[j]->getComponentID().toRawUTF8());
                ++failures;
            }
        }
    }
}

void ym2612_panel_shows_only_its_controls()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    const auto visible = [&](const char* id)
    {
        auto* component = findById(*editor, id);
        CHECK(component != nullptr);
        return component != nullptr && component->isVisible();
    };

    const char* const own2a03[] = { retrochip::ids::channel, retrochip::ids::duty, retrochip::ids::noiseMode,
                                    retrochip::ids::attack, retrochip::ids::decay, retrochip::ids::sustain,
                                    retrochip::ids::release };
    const char* const ownYm[] = { retrochip::ids::genPatch, retrochip::ids::genAlgorithm, retrochip::ids::genFeedback,
                                  retrochip::ids::genBrightness, retrochip::ids::genAttack,
                                  retrochip::ids::genDecay, retrochip::ids::genRelease };
    const char* const shared[] = { retrochip::ids::vibratoRate, retrochip::ids::vibratoDepth, retrochip::ids::glide,
                                   retrochip::ids::velocity, retrochip::ids::gain, "console", "preset" };

    // 2A03 first.
    for (const char* id : own2a03)
        CHECK(visible(id));
    for (const char* id : ownYm)
        CHECK(!visible(id));
    checkLayout(*editor);

    // A 2A03 channel that greys out the shared controls must not do the same on the YM2612.
    setParam(processor, retrochip::ids::channel, 2.0f);
    checkEnabled(*editor, retrochip::ids::vibratoRate, false);

    setParam(processor, kConsoleId, 1.0f);
    for (const char* id : own2a03)
        CHECK(!visible(id));
    for (const char* id : ownYm)
        CHECK(visible(id));
    for (const char* id : shared)
        CHECK(visible(id));
    for (const char* id : { retrochip::ids::vibratoRate, retrochip::ids::vibratoDepth, retrochip::ids::glide,
                            retrochip::ids::velocity, retrochip::ids::gain, retrochip::ids::genPatch,
                            retrochip::ids::genAlgorithm, retrochip::ids::genFeedback })
        checkEnabled(*editor, id, true);
    checkTitles(*editor);
    checkLayout(*editor);

    // And back: the 2A03 rule applies again.
    setParam(processor, kConsoleId, 0.0f);
    for (const char* id : own2a03)
        CHECK(visible(id));
    for (const char* id : ownYm)
        CHECK(!visible(id));
    checkEnabled(*editor, retrochip::ids::vibratoRate, false);
    checkEnabled(*editor, retrochip::ids::glide, false);
    checkEnabled(*editor, retrochip::ids::duty, false);
    checkLayout(*editor);

    // S-DSP: nothing of either console yet, but the shared header stays.
    setParam(processor, kConsoleId, 2.0f);
    for (const char* id : ownYm)
        CHECK(!visible(id));
    CHECK(visible("console"));
}

void ym2612_snapshot()
{
    RetroChipProcessor processor;
    processor.setCurrentProgram(17);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    CHECK(editor->getWidth() == 760);
    CHECK(editor->getHeight() == 520);
    checkTitles(*editor);
    checkLayout(*editor);
    saveSnapshot(*editor, "editor_ym2612.png");
}

void console_switch_with_held_chip_notes_is_safe()
{
    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    selectYm2612(processor, 18);
    CHECK(run(processor, 6, 64) > 0.01);

    // Leaving the YM2612 stops its notes; nothing comes back with it.
    setConsole(processor, 0);
    run(processor, 2);
    CHECK(processor.getActiveConsole() == Console::Chip2A03);
    setConsole(processor, 1);
    // Nothing is left, from the first block back.
    CHECK(run(processor, 6) < 1e-3);
    auto* genesis = dynamic_cast<GenesisEngine*>(processor.engineFor(Console::Ym2612));
    CHECK(genesis != nullptr && genesis->activeVoices() == 0);
}

// ---- S-DSP ----

using retrochip::SnesEngine;
using retrochip::SnesParams;

void selectSdsp(RetroChipProcessor& processor, int program = 24)
{
    processor.setCurrentProgram(program);
    CHECK(processor.getActiveConsole() == Console::SDsp);
}

// Plays note 60 for `blocks` blocks and returns channel 0.
std::vector<float> renderSnesNote(RetroChipProcessor& processor, int blocks = 12)
{
    processor.prepareToPlay(kSampleRate, kBlock);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 0);
    return processWith(processor, blocks, on);
}

void sdsp_note_produces_sound()
{
    RetroChipProcessor processor;
    selectSdsp(processor, 24);
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(run(processor, 2) < 1e-4); // idle voices are silent
    CHECK(run(processor, 8, 69) > 0.02);

    // The note ends, and nothing is left hanging.
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer off;
    off.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
    processor.processBlock(buffer, off);
    run(processor, 200);
    CHECK(run(processor, 4) < 1e-3);

    // The pedal keeps a released note sounding, and all-notes-off ends it.
    juce::MidiBuffer hold;
    hold.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    hold.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(120)), 1);
    hold.addEvent(juce::MidiMessage::noteOff(1, 60), 2);
    processor.processBlock(buffer, hold);
    CHECK(run(processor, 4) > 0.01);
    allOff(processor);
    run(processor, 200);
    CHECK(run(processor, 4) < 1e-3);

    // Gain is shared with the other consoles.
    const auto levelAt = [](float gainDb)
    {
        RetroChipProcessor p;
        selectSdsp(p);
        setParam(p, retrochip::ids::gain, gainDb);
        p.prepareToPlay(kSampleRate, kBlock);
        run(p, 2);
        return run(p, 8, 69);
    };
    const double ratio = levelAt(-12.0f) / levelAt(0.0f);
    CHECK(ratio > 0.2 && ratio < 0.3);

    // "My sample" with nothing imported is silent.
    RetroChipProcessor empty;
    selectSdsp(empty);
    setParam(empty, retrochip::ids::snesInstrument, 12.0f);
    empty.prepareToPlay(kSampleRate, kBlock);
    CHECK(run(empty, 8, 60) == 0.0);
}

void thirty_six_presets_all_sound()
{
    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(processor.getNumPrograms() == 72);

    juce::StringArray seen;
    for (int i = 0; i < 72; ++i)
    {
        processor.setCurrentProgram(i);
        CHECK(processor.getCurrentProgram() == i);
        const Console consoles[] = { Console::Chip2A03, Console::Ym2612, Console::SDsp, Console::Psg, Console::Vrc6, Console::N163 };
        CHECK(processor.getActiveConsole() == consoles[i / 12]);

        const auto name = processor.getProgramName(i);
        CHECK(name.isNotEmpty());
        CHECK(!seen.contains(name));
        seen.add(name);
        for (const char* forbidden : { "nes", "nintendo", "genesis", "snes", "sega", "super famicom" })
            CHECK(!name.containsIgnoreCase(forbidden));

        allOff(processor);
        if (run(processor, 40, 60) <= 0.005)
        {
            std::printf("  FAILED: preset %s is silent\n", name.toRawUTF8());
            ++failures;
        }
        allOff(processor);
    }
    CHECK(processor.getProgramName(72).isEmpty());

    // The 12 S-DSP presets each use a bank instrument, all different, and the echo.
    juce::Array<int> instruments;
    for (int i = 24; i < 36; ++i)
    {
        processor.setCurrentProgram(i);
        const int instrument = juce::roundToInt(getParam(processor, retrochip::ids::snesInstrument));
        CHECK(instrument >= 0 && instrument < 12);
        CHECK(!instruments.contains(instrument));
        instruments.add(instrument);
    }
}

void sdsp_preset_sets_its_parameters()
{
    RetroChipProcessor processor;
    setParam(processor, retrochip::ids::vibratoDepth, 77.0f);
    setParam(processor, retrochip::ids::glide, 400.0f);
    setParam(processor, retrochip::ids::snesRootNote, 50.0f);
    setParam(processor, retrochip::ids::snesLoop, 1.0f);
    for (int i = 24; i < 36; ++i)
    {
        processor.setCurrentProgram(i);
        const auto& preset = retrochip::kPresets[static_cast<size_t>(i)];
        CHECK(preset.console == 2);
        CHECK(getParam(processor, retrochip::ids::snesInstrument) == static_cast<float>(preset.snesInstrument));
        CHECK(getParam(processor, retrochip::ids::snesAttack) == static_cast<float>(preset.snesAttack));
        CHECK(getParam(processor, retrochip::ids::snesDecay) == static_cast<float>(preset.snesDecay));
        CHECK(getParam(processor, retrochip::ids::snesSustainLevel) == static_cast<float>(preset.snesSustainLevel));
        CHECK(getParam(processor, retrochip::ids::snesSustainRate) == static_cast<float>(preset.snesSustainRate));
        CHECK(std::abs(getParam(processor, retrochip::ids::snesRelease) - preset.snesRelease) < 1.5f);
        CHECK(std::abs(getParam(processor, retrochip::ids::snesEchoMix) - preset.snesEchoMix) < 0.6f);
        CHECK(getParam(processor, retrochip::ids::snesEchoDelay) == static_cast<float>(preset.snesEchoDelay));
        CHECK(std::abs(getParam(processor, retrochip::ids::snesEchoFeedback) - preset.snesEchoFeedback) < 0.6f);
        CHECK(std::abs(getParam(processor, retrochip::ids::vibratoRate) - preset.vibratoRate) < 0.06f);
        CHECK(std::abs(getParam(processor, retrochip::ids::vibratoDepth) - preset.vibratoDepth) < 0.6f);
        CHECK(std::abs(getParam(processor, retrochip::ids::glide) - preset.glide) < 1.0f);
        // The root note and the loop belong to the user's sample, so a preset leaves them alone.
        CHECK(getParam(processor, retrochip::ids::snesRootNote) == 50.0f);
        CHECK(getParam(processor, retrochip::ids::snesLoop) == 1.0f);
    }
}

void preset_changes_console_with_held_notes()
{
    // From each console to a preset of each other console, with notes held and the pedal down.
    for (int from = 0; from < 3; ++from)
        for (int to = 0; to < 3; ++to)
        {
            RetroChipProcessor processor;
            processor.prepareToPlay(kSampleRate, kBlock);
            processor.setCurrentProgram(from * 12);
            juce::MidiBuffer hold;
            hold.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
            hold.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 1);
            hold.addEvent(juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(110)), 2);
            processWith(processor, 6, hold);

            processor.setCurrentProgram(to * 12 + 1);
            CHECK(processor.getActiveConsole() == static_cast<Console>(to));
            run(processor, 3);

            allOff(processor);
            run(processor, 200);
            const double tail = run(processor, 6);
            if (!(tail < 1e-3))
            {
                std::printf("  FAILED: stuck notes after preset %d to %d (%g)\n", from, to, tail);
                ++failures;
            }

            // Back on the console that was left, nothing comes back. A chip that was stopped while idle lets
            // its last output (the resampler's history, the DC blocker) settle once it runs again.
            processor.setCurrentProgram(from * 12 + 1);
            run(processor, 20);
            const double back = run(processor, 8);
            if (!(back < 1e-3))
            {
                std::printf("  FAILED: sound after returning from %d to %d (%g)\n", to, from, back);
                ++failures;
            }
            for (const auto which : { Console::Chip2A03, Console::Ym2612, Console::SDsp })
                if (auto* engine = processor.engineFor(which))
                    if (auto* snes = dynamic_cast<SnesEngine*>(engine))
                        CHECK(snes->activeVoices() == 0);
        }
}

void sdsp_defaults_match_the_spec()
{
    RetroChipProcessor processor;
    CHECK(getParam(processor, retrochip::ids::snesInstrument) == 0.0f);
    CHECK(getParam(processor, retrochip::ids::snesAttack) == 15.0f);
    CHECK(getParam(processor, retrochip::ids::snesDecay) == 7.0f);
    CHECK(getParam(processor, retrochip::ids::snesSustainLevel) == 7.0f);
    CHECK(getParam(processor, retrochip::ids::snesSustainRate) == 0.0f);
    CHECK(getParam(processor, retrochip::ids::snesRelease) == 8.0f);
    CHECK(getParam(processor, retrochip::ids::snesEchoMix) == 0.0f);
    CHECK(getParam(processor, retrochip::ids::snesEchoDelay) == 4.0f);
    CHECK(std::abs(getParam(processor, retrochip::ids::snesEchoFeedback) - 30.0f) < 1e-4f); // a float, stored as 0.3
    CHECK(getParam(processor, retrochip::ids::snesRootNote) == 60.0f);
    CHECK(getParam(processor, retrochip::ids::snesLoop) == 0.0f);

    const auto range = [&](const char* id)
    {
        auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(processor.apvts.getParameter(id));
        CHECK(parameter != nullptr);
        return parameter != nullptr ? parameter->getNormalisableRange().getRange() : juce::Range<float>();
    };
    CHECK(range(retrochip::ids::snesInstrument) == juce::Range<float>(0.0f, 12.0f));
    CHECK(range(retrochip::ids::snesAttack) == juce::Range<float>(0.0f, 15.0f));
    CHECK(range(retrochip::ids::snesDecay) == juce::Range<float>(0.0f, 7.0f));
    CHECK(range(retrochip::ids::snesSustainLevel) == juce::Range<float>(0.0f, 7.0f));
    CHECK(range(retrochip::ids::snesSustainRate) == juce::Range<float>(0.0f, 31.0f));
    CHECK(range(retrochip::ids::snesRelease) == juce::Range<float>(8.0f, 2000.0f));
    CHECK(range(retrochip::ids::snesEchoMix) == juce::Range<float>(0.0f, 100.0f));
    CHECK(range(retrochip::ids::snesEchoDelay) == juce::Range<float>(0.0f, 15.0f));
    CHECK(range(retrochip::ids::snesEchoFeedback) == juce::Range<float>(0.0f, 100.0f));
    CHECK(range(retrochip::ids::snesRootNote) == juce::Range<float>(0.0f, 127.0f));

    // The instrument menu is the bank in order, then the user's sample.
    auto* instrument = dynamic_cast<juce::AudioParameterChoice*>(processor.apvts.getParameter(retrochip::ids::snesInstrument));
    CHECK(instrument != nullptr);
    if (instrument != nullptr)
    {
        const auto& bank = retrochip::sharedSnesBank();
        CHECK(bank.size() == 12);
        CHECK(instrument->choices.size() == 13);
        for (size_t i = 0; i < bank.size(); ++i)
            CHECK(instrument->choices[static_cast<int>(i)] == juce::String::fromUTF8(bank[i]->name.c_str()));
        CHECK(instrument->choices[0] == juce::String("Strings"));
        CHECK(instrument->choices[12] == juce::String("My sample"));
    }

    auto* loop = dynamic_cast<juce::AudioParameterBool*>(processor.apvts.getParameter(retrochip::ids::snesLoop));
    CHECK(loop != nullptr);
}

void sdsp_knobs_show_chip_times()
{
    RetroChipProcessor processor;
    const auto textAt = [&](const char* id, int step, int steps)
    {
        auto* parameter = processor.apvts.getParameter(id);
        CHECK(parameter != nullptr);
        return parameter != nullptr ? parameter->getText(static_cast<float>(step) / static_cast<float>(steps), 100) : juce::String();
    };

    CHECK(textAt(retrochip::ids::snesAttack, 0, 15) == "4.1 s");
    CHECK(textAt(retrochip::ids::snesAttack, 4, 15) == "640 ms");
    CHECK(textAt(retrochip::ids::snesAttack, 15, 15) == "0 ms");
    CHECK(textAt(retrochip::ids::snesDecay, 0, 7) == "1.2 s");
    CHECK(textAt(retrochip::ids::snesDecay, 7, 7) == "37 ms");
    CHECK(textAt(retrochip::ids::snesSustainLevel, 0, 7) == "1/8");
    CHECK(textAt(retrochip::ids::snesSustainLevel, 7, 7) == "8/8");
    CHECK(textAt(retrochip::ids::snesSustainRate, 0, 31) == "Infinite");
    CHECK(textAt(retrochip::ids::snesSustainRate, 1, 31) == "38 s");
    CHECK(textAt(retrochip::ids::snesSustainRate, 16, 31) == "1.2 s");
    CHECK(textAt(retrochip::ids::snesSustainRate, 31, 31) == "18 ms");
    CHECK(textAt(retrochip::ids::snesEchoDelay, 0, 15) == "0 ms");
    CHECK(textAt(retrochip::ids::snesEchoDelay, 4, 15) == "64 ms");
    CHECK(textAt(retrochip::ids::snesEchoDelay, 15, 15) == "240 ms");
    CHECK(textAt(retrochip::ids::snesRootNote, 60, 127) == "C4");

    // Every attack step reads out the chip's table, in order, and the knob shows the same text.
    const char* const attack[16] = { "4.1 s", "2.5 s", "1.5 s", "1.0 s", "640 ms", "380 ms", "260 ms", "160 ms",
                                     "96 ms", "64 ms", "40 ms", "24 ms", "16 ms", "10 ms", "6 ms", "0 ms" };
    for (int i = 0; i < 16; ++i)
        CHECK(textAt(retrochip::ids::snesAttack, i, 15) == attack[i]);

    // Typing a shown value picks its step.
    auto* sustainRate = processor.apvts.getParameter(retrochip::ids::snesSustainRate);
    CHECK(sustainRate != nullptr && sustainRate->getValueForText("1.2 s") == 16.0f / 31.0f);

    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    setParam(processor, kConsoleId, 2.0f);
    setParam(processor, retrochip::ids::snesAttack, 0.0f);
    setParam(processor, retrochip::ids::snesEchoDelay, 4.0f);
    const auto knobText = [&](const char* id)
    {
        auto* knob = findById(*editor, id);
        CHECK(knob != nullptr);
        for (auto* child : knob != nullptr ? knob->getChildren() : juce::Array<juce::Component*>())
            if (auto* slider = dynamic_cast<juce::Slider*>(child))
                return slider->getTextFromValue(slider->getValue());
        CHECK(false);
        return juce::String();
    };
    CHECK(knobText(retrochip::ids::snesAttack) == "4.1 s");
    CHECK(knobText(retrochip::ids::snesEchoDelay) == "64 ms");
    setParam(processor, retrochip::ids::snesAttack, 15.0f);
    CHECK(knobText(retrochip::ids::snesAttack) == "0 ms");
}

void sdsp_parameters_reach_the_engine()
{
    const auto render = [](const char* id, float value, int program)
    {
        RetroChipProcessor processor;
        selectSdsp(processor, program);
        if (id != nullptr)
            setParam(processor, id, value);
        return renderSnesNote(processor);
    };

    // Warm Strings: every control changes the sound.
    const auto base = render(nullptr, 0.0f, 24);
    CHECK(peakOf(base) > 0.02);
    CHECK(maxDifference(base, render(retrochip::ids::snesInstrument, 2.0f, 24)) > 0.01);
    CHECK(maxDifference(base, render(retrochip::ids::snesEchoMix, 100.0f, 24)) > 0.005);
    CHECK(maxDifference(base, render(retrochip::ids::snesEchoDelay, 12.0f, 24)) > 0.005);
    CHECK(maxDifference(base, render(retrochip::ids::snesSustainLevel, 1.0f, 24)) > 0.005);
    CHECK(maxDifference(base, render(retrochip::ids::snesSustainRate, 20.0f, 24)) > 0.005);
    CHECK(maxDifference(base, render(retrochip::ids::vibratoDepth, 100.0f, 24)) > 0.0);
    CHECK(maxDifference(base, render(retrochip::ids::velocity, 0.0f, 24)) > 0.01);

    // Feedback needs a repeat of the echo to show, so use a short delay: 16 ms, then 100 % against 0 %.
    const auto withFeedback = [&](float feedback)
    {
        RetroChipProcessor processor;
        selectSdsp(processor, 24);
        setParam(processor, retrochip::ids::snesEchoDelay, 1.0f);
        setParam(processor, retrochip::ids::snesEchoMix, 100.0f);
        setParam(processor, retrochip::ids::snesEchoFeedback, feedback);
        return renderSnesNote(processor);
    };
    CHECK(maxDifference(withFeedback(0.0f), withFeedback(100.0f)) > 0.005);

    // Decay shows when the sustain level is below the top: Brass Stab sustains at 5/8.
    const auto brass = [&](float decay)
    {
        RetroChipProcessor processor;
        selectSdsp(processor, 25);
        setParam(processor, retrochip::ids::snesDecay, decay);
        return renderSnesNote(processor);
    };
    CHECK(maxDifference(brass(0.0f), brass(7.0f)) > 0.005);

    // A slower attack is quieter in the first 20 ms.
    const auto early = [&](float attack) { return peakOf(render(retrochip::ids::snesAttack, attack, 29), 0, 960); };
    CHECK(early(4.0f) < 0.5 * early(15.0f));

    // A short release ends a released note sooner.
    const auto tailAfterOff = [](float release)
    {
        RetroChipProcessor processor;
        selectSdsp(processor, 26);
        setParam(processor, retrochip::ids::snesRelease, release);
        processor.prepareToPlay(kSampleRate, kBlock);
        juce::MidiBuffer on;
        on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 0);
        processWith(processor, 4, on);
        juce::MidiBuffer off;
        off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        processWith(processor, 1, off);
        return peakOf(processWith(processor, 6));
    };
    CHECK(tailAfterOff(8.0f) < 0.5 * tailAfterOff(2000.0f));

    // The root note and loop belong to the user's sample: a bank instrument ignores them.
    CHECK(maxDifference(base, render(retrochip::ids::snesRootNote, 30.0f, 24)) == 0.0);
    CHECK(maxDifference(base, render(retrochip::ids::snesLoop, 1.0f, 24)) == 0.0);

    // The S-DSP's controls leave the 2A03 alone.
    const auto render2A03 = [](float attack)
    {
        RetroChipProcessor processor;
        setParam(processor, retrochip::ids::snesAttack, attack);
        setParam(processor, retrochip::ids::snesEchoMix, 100.0f);
        processor.prepareToPlay(kSampleRate, kBlock);
        juce::MidiBuffer on;
        on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 0);
        return processWith(processor, 8, on);
    };
    CHECK(maxDifference(render2A03(0.0f), render2A03(15.0f)) == 0.0);
}

void sdsp_plugin_drives_the_engine_with_the_parameters()
{
    // The plugin must give the engine what its parameters say.
    SnesParams params;
    params.instrument = 7;
    params.attack = 9;
    params.decay = 3;
    params.sustainLevel = 4;
    params.sustainRate = 12;
    params.releaseMs = 300.0f;
    params.echoMix = 0.4f;
    params.echoDelay = 6;
    params.echoFeedback = 0.6f;
    params.vibratoRateHz = 6.0f;
    params.vibratoDepthCents = 20.0f;
    params.glideMs = 0.0f;
    params.velocityOn = true;

    SnesEngine engine;
    engine.setBank(&retrochip::sharedSnesBank());
    engine.setParams(params);
    engine.prepare(kSampleRate);
    engine.noteOn(60, 110);
    std::vector<float> direct(static_cast<size_t>(12 * kBlock));
    for (int block = 0; block < 12; ++block)
        engine.render(direct.data() + block * kBlock, kBlock);

    const auto setAll = [](RetroChipProcessor& p)
    {
        setParam(p, retrochip::ids::snesInstrument, 7.0f);
        setParam(p, retrochip::ids::snesAttack, 9.0f);
        setParam(p, retrochip::ids::snesDecay, 3.0f);
        setParam(p, retrochip::ids::snesSustainLevel, 4.0f);
        setParam(p, retrochip::ids::snesSustainRate, 12.0f);
        setParam(p, retrochip::ids::snesRelease, 300.0f);
        setParam(p, retrochip::ids::snesEchoMix, 40.0f);
        setParam(p, retrochip::ids::snesEchoDelay, 6.0f);
        setParam(p, retrochip::ids::snesEchoFeedback, 60.0f);
        setParam(p, retrochip::ids::vibratoRate, 6.0f);
        setParam(p, retrochip::ids::vibratoDepth, 20.0f);
        setParam(p, retrochip::ids::glide, 0.0f);
    };

    RetroChipProcessor processor;
    selectSdsp(processor, 24);
    setAll(processor);
    const auto viaPlugin = renderSnesNote(processor);

    CHECK(peakOf(direct) > 0.02);
    CHECK(maxDifference(direct, viaPlugin) < 1e-4);

    // The same when the parameters change after prepareToPlay, as automation does while playing.
    RetroChipProcessor automated;
    automated.prepareToPlay(kSampleRate, kBlock);
    selectSdsp(automated, 24);
    processWith(automated, 2);
    setAll(automated);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 0);
    const auto viaAutomation = processWith(automated, 12, on);

    // The engine's 60 Hz clock ran through the two silent blocks, so the reference does the same.
    SnesEngine later;
    later.setBank(&retrochip::sharedSnesBank());
    later.prepare(kSampleRate);
    std::vector<float> silent(static_cast<size_t>(kBlock));
    later.render(silent.data(), kBlock);
    later.render(silent.data(), kBlock);
    later.setParams(params);
    later.noteOn(60, 110);
    std::vector<float> reference(static_cast<size_t>(12 * kBlock));
    for (int block = 0; block < 12; ++block)
        later.render(reference.data() + block * kBlock, kBlock);
    CHECK(maxDifference(reference, viaAutomation) < 1e-4);
}

void sdsp_instruments_share_one_bank()
{
    // Two plugins at once use the same samples, and each plays on its own.
    RetroChipProcessor a;
    RetroChipProcessor b;
    selectSdsp(a, 24);
    selectSdsp(b, 24);
    const auto fromA = renderSnesNote(a);
    const auto fromB = renderSnesNote(b);
    CHECK(peakOf(fromA) > 0.02);
    CHECK(maxDifference(fromA, fromB) == 0.0);
}

void sdsp_instrument_change_stops_held_notes()
{
    RetroChipProcessor processor;
    selectSdsp(processor, 26);
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(run(processor, 6, 60) > 0.02);
    setParam(processor, retrochip::ids::snesInstrument, 3.0f);
    run(processor, 2);
    auto* snes = dynamic_cast<SnesEngine*>(processor.engineFor(Console::SDsp));
    CHECK(snes != nullptr && snes->activeVoices() == 0);
}

void sdsp_state_round_trip()
{
    juce::MemoryBlock saved;
    std::vector<float> expected;
    {
        RetroChipProcessor processor;
        processor.setCurrentProgram(30);
        setParam(processor, retrochip::ids::snesInstrument, 4.0f);
        setParam(processor, retrochip::ids::snesAttack, 10.0f);
        setParam(processor, retrochip::ids::snesDecay, 2.0f);
        setParam(processor, retrochip::ids::snesSustainLevel, 3.0f);
        setParam(processor, retrochip::ids::snesSustainRate, 9.0f);
        setParam(processor, retrochip::ids::snesRelease, 640.0f);
        setParam(processor, retrochip::ids::snesEchoMix, 35.0f);
        setParam(processor, retrochip::ids::snesEchoDelay, 9.0f);
        setParam(processor, retrochip::ids::snesEchoFeedback, 70.0f);
        setParam(processor, retrochip::ids::snesRootNote, 48.0f);
        setParam(processor, retrochip::ids::snesLoop, 1.0f);
        processor.getStateInformation(saved);
        expected = renderSnesNote(processor);
    }

    RetroChipProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    CHECK(restored.getActiveConsole() == Console::SDsp);
    CHECK(restored.getCurrentProgram() == 30);
    CHECK(getParam(restored, retrochip::ids::snesInstrument) == 4.0f);
    CHECK(getParam(restored, retrochip::ids::snesAttack) == 10.0f);
    CHECK(getParam(restored, retrochip::ids::snesDecay) == 2.0f);
    CHECK(getParam(restored, retrochip::ids::snesSustainLevel) == 3.0f);
    CHECK(getParam(restored, retrochip::ids::snesSustainRate) == 9.0f);
    CHECK(std::abs(getParam(restored, retrochip::ids::snesRelease) - 640.0f) < 1.5f);
    CHECK(std::abs(getParam(restored, retrochip::ids::snesEchoMix) - 35.0f) < 0.5f);
    CHECK(getParam(restored, retrochip::ids::snesEchoDelay) == 9.0f);
    CHECK(std::abs(getParam(restored, retrochip::ids::snesEchoFeedback) - 70.0f) < 0.5f);
    CHECK(getParam(restored, retrochip::ids::snesRootNote) == 48.0f);
    CHECK(getParam(restored, retrochip::ids::snesLoop) == 1.0f);

    // The restored plugin sounds like the saved one.
    const auto again = renderSnesNote(restored);
    CHECK(peakOf(again) > 0.02);
    CHECK(maxDifference(expected, again) < 1e-6);
}

void sdsp_panel_shows_only_its_controls()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    const auto visible = [&](const char* id)
    {
        auto* component = findById(*editor, id);
        CHECK(component != nullptr);
        return component != nullptr && component->isVisible();
    };

    const char* const own2a03[] = { retrochip::ids::channel, retrochip::ids::duty, retrochip::ids::noiseMode,
                                    retrochip::ids::attack, retrochip::ids::decay, retrochip::ids::sustain,
                                    retrochip::ids::release };
    const char* const ownYm[] = { retrochip::ids::genPatch, retrochip::ids::genAlgorithm, retrochip::ids::genFeedback,
                                  retrochip::ids::genBrightness, retrochip::ids::genAttack,
                                  retrochip::ids::genDecay, retrochip::ids::genRelease };
    const char* const ownSdsp[] = { retrochip::ids::snesInstrument, retrochip::ids::snesAttack,
                                    retrochip::ids::snesDecay, retrochip::ids::snesSustainLevel,
                                    retrochip::ids::snesSustainRate, retrochip::ids::snesRelease,
                                    retrochip::ids::snesEchoMix, retrochip::ids::snesEchoDelay,
                                    retrochip::ids::snesEchoFeedback, retrochip::ids::snesRootNote,
                                    retrochip::ids::snesLoop, "snesImport", "snesSampleStatus" };
    const char* const shared[] = { retrochip::ids::vibratoRate, retrochip::ids::vibratoDepth, retrochip::ids::glide,
                                   retrochip::ids::velocity, retrochip::ids::gain, "console", "preset" };

    // None of the S-DSP's controls show on the other two consoles.
    for (const char* id : ownSdsp)
        CHECK(!visible(id));
    setParam(processor, kConsoleId, 1.0f);
    for (const char* id : ownSdsp)
        CHECK(!visible(id));

    setParam(processor, kConsoleId, 2.0f);
    for (const char* id : own2a03)
        CHECK(!visible(id));
    for (const char* id : ownYm)
        CHECK(!visible(id));
    for (const char* id : ownSdsp)
        CHECK(visible(id));
    for (const char* id : shared)
        CHECK(visible(id));
    for (const char* id : { retrochip::ids::vibratoRate, retrochip::ids::vibratoDepth, retrochip::ids::glide,
                            retrochip::ids::velocity, retrochip::ids::gain, retrochip::ids::snesInstrument,
                            retrochip::ids::snesAttack, retrochip::ids::snesDecay, retrochip::ids::snesSustainLevel,
                            retrochip::ids::snesSustainRate, retrochip::ids::snesRelease, retrochip::ids::snesEchoMix,
                            retrochip::ids::snesEchoDelay, retrochip::ids::snesEchoFeedback })
        checkEnabled(*editor, id, true);
    checkTitles(*editor);
    checkLayout(*editor);

    // The root note and the loop apply to the user's sample only: grey for a bank instrument.
    checkEnabled(*editor, retrochip::ids::snesRootNote, false);
    checkEnabled(*editor, retrochip::ids::snesLoop, false);
    setParam(processor, retrochip::ids::snesInstrument, 12.0f);
    checkEnabled(*editor, retrochip::ids::snesRootNote, true);
    checkEnabled(*editor, retrochip::ids::snesLoop, true);
    checkLayout(*editor);
    setParam(processor, retrochip::ids::snesInstrument, 5.0f);
    checkEnabled(*editor, retrochip::ids::snesRootNote, false);
    checkEnabled(*editor, retrochip::ids::snesLoop, false);

    // A 2A03 channel that greys out the shared controls must not do the same on the S-DSP.
    setParam(processor, kConsoleId, 0.0f);
    setParam(processor, retrochip::ids::channel, 2.0f);
    checkEnabled(*editor, retrochip::ids::vibratoRate, false);
    setParam(processor, kConsoleId, 2.0f);
    for (const char* id : { retrochip::ids::vibratoRate, retrochip::ids::vibratoDepth, retrochip::ids::glide,
                            retrochip::ids::velocity })
        checkEnabled(*editor, id, true);

    // The shared knobs are compact on the S-DSP only.
    const auto isCompact = [&](const char* id)
    {
        auto* knob = findById(*editor, id);
        CHECK(knob != nullptr);
        for (auto* child : knob != nullptr ? knob->getChildren() : juce::Array<juce::Component*>())
            if (dynamic_cast<juce::Slider*>(child) != nullptr)
                return static_cast<bool>(child->getProperties()["compact"]);
        CHECK(false);
        return false;
    };
    CHECK(isCompact(retrochip::ids::gain));
    CHECK(isCompact(retrochip::ids::glide));

    // And back to the 2A03.
    setParam(processor, kConsoleId, 0.0f);
    CHECK(!isCompact(retrochip::ids::gain));
    CHECK(!isCompact(retrochip::ids::glide));
    for (const char* id : ownSdsp)
        CHECK(!visible(id));
    for (const char* id : own2a03)
        CHECK(visible(id));
    checkEnabled(*editor, retrochip::ids::vibratoRate, false);
    checkLayout(*editor);
}

void sdsp_instrument_menu_changes_the_parameter()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    setParam(processor, kConsoleId, 2.0f);
    auto* box = dynamic_cast<juce::ComboBox*>(findById(*editor, retrochip::ids::snesInstrument));
    CHECK(box != nullptr);
    if (box == nullptr)
        return;
    CHECK(box->getNumItems() == 13);
    CHECK(box->getItemText(12) == juce::String("My sample"));
    box->setSelectedItemIndex(6, juce::sendNotificationSync);
    CHECK(getParam(processor, retrochip::ids::snesInstrument) == 6.0f);
    setParam(processor, retrochip::ids::snesInstrument, 9.0f);
    CHECK(box->getSelectedItemIndex() == 9);
}

// ---- WAV import ----

// A folder of test files, removed with it.
struct TempFolder
{
    TempFolder()
    {
        static int counter = 0;
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getChildFile("retrochip_import_" + juce::String(juce::Time::currentTimeMillis()) + "_" + juce::String(counter++));
        CHECK(dir.createDirectory().wasOk());
    }
    ~TempFolder() { dir.deleteRecursively(); }

    juce::File dir;
};

std::vector<float> sineWave(double frequency, double rate, double seconds, float amplitude = 0.5f)
{
    std::vector<float> samples(static_cast<size_t>(std::llround(rate * seconds)));
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = amplitude * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * frequency * static_cast<double>(i) / rate));
    return samples;
}

void writeWav(const juce::File& file, const std::vector<std::vector<float>>& channels, double rate, int bits)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    CHECK(stream != nullptr);
    if (stream == nullptr)
        return;
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriter::Options {}
                                                  .withSampleRate(rate)
                                                  .withNumChannels(static_cast<int>(channels.size()))
                                                  .withBitsPerSample(bits));
    CHECK(writer != nullptr);
    if (writer == nullptr)
        return;
    const int length = static_cast<int>(channels.front().size());
    juce::AudioBuffer<float> buffer(static_cast<int>(channels.size()), length);
    for (int c = 0; c < buffer.getNumChannels(); ++c)
        for (int i = 0; i < length; ++i)
            buffer.setSample(c, i, channels[static_cast<size_t>(c)][static_cast<size_t>(i)]);
    CHECK(writer->writeFromAudioSampleBuffer(buffer, 0, length));
    writer.reset(); // finishes the header
}

void writeMono(const juce::File& file, const std::vector<float>& samples, double rate = 32000.0, int bits = 16)
{
    writeWav(file, { samples }, rate, bits);
}

// The frequency of a signal from its rising zero crossings, from `from` on.
double estimateFrequency(const std::vector<float>& samples, double rate, size_t from)
{
    std::vector<double> crossings;
    for (size_t i = std::max<size_t>(from, 1); i < samples.size(); ++i)
        if (samples[i - 1] < 0.0f && samples[i] >= 0.0f)
            crossings.push_back(static_cast<double>(i - 1) + samples[i - 1] / (samples[i - 1] - samples[i]));
    if (crossings.size() < 3)
        return 0.0;
    return static_cast<double>(crossings.size() - 1) * rate / (crossings.back() - crossings.front());
}

// An S-DSP processor playing the user's sample as it is: instant attack, full sustain, no echo (the preset
// it starts from has its own), with its root note at 69 so that a 440 Hz sample at the chip's rate sounds at
// 440 Hz on note 69.
void useUserSample(RetroChipProcessor& processor)
{
    selectSdsp(processor);
    setParam(processor, retrochip::ids::snesInstrument, 12.0f);
    setParam(processor, retrochip::ids::snesRootNote, 69.0f);
    setParam(processor, retrochip::ids::snesAttack, 15.0f);
    setParam(processor, retrochip::ids::snesDecay, 7.0f);
    setParam(processor, retrochip::ids::snesSustainLevel, 7.0f);
    setParam(processor, retrochip::ids::snesSustainRate, 0.0f);
    setParam(processor, retrochip::ids::snesEchoMix, 0.0f);
    setParam(processor, retrochip::ids::vibratoDepth, 0.0f);
    setParam(processor, retrochip::ids::glide, 0.0f);
}

std::vector<float> playUserNote(RetroChipProcessor& processor, int note, int blocks)
{
    processor.prepareToPlay(kSampleRate, kBlock);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(110)), 0);
    return processWith(processor, blocks, on);
}

const juce::String kImportPrompt = "Import a WAV file to play this sound";

void import_valid_wav_plays()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("tone.wav");
    writeMono(file, sineWave(440.0, 32000.0, 0.5));

    RetroChipProcessor processor;
    useUserSample(processor);
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(run(processor, 4, 69) == 0.0); // nothing imported yet
    allOff(processor);

    // Imported before playback starts, it sounds from the next block.
    CHECK(processor.importSample(file).isEmpty());
    CHECK(processor.getUserSampleName() == "tone");
    CHECK(processor.getSampleStatus() == "tone");
    const auto played = playUserNote(processor, 69, 20);
    CHECK(peakOf(played) > 0.05);
    CHECK(peakOf(played) < 0.8);
    const double f69 = estimateFrequency(played, kSampleRate, 2 * kBlock);
    CHECK(std::abs(f69 - 440.0) < 4.4);

    // The root note sets the pitch: an octave up on the key an octave higher.
    const double f81 = estimateFrequency(playUserNote(processor, 81, 20), kSampleRate, 2 * kBlock);
    CHECK(std::abs(f81 - 880.0) < 8.8);

    // Importing never touches the other instruments.
    setParam(processor, retrochip::ids::snesInstrument, 2.0f);
    CHECK(peakOf(playUserNote(processor, 69, 8)) > 0.02);
}

void import_preserves_pitch_at_every_rate()
{
    TempFolder folder;
    for (const double rate : { 11025.0, 22050.0, 32000.0, 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const auto file = folder.dir.getChildFile("tone_" + juce::String(static_cast<int>(rate)) + ".wav");
        writeMono(file, sineWave(440.0, rate, 0.4), rate, 24);
        RetroChipProcessor processor;
        useUserSample(processor);
        CHECK(processor.importSample(file).isEmpty());
        const auto played = playUserNote(processor, 69, 16);
        const double frequency = estimateFrequency(played, kSampleRate, 2 * kBlock);
        if (std::abs(frequency - 440.0) >= 4.4)
            std::printf("  rate %g: %g Hz\n", rate, frequency);
        CHECK(std::abs(frequency - 440.0) < 4.4);
        CHECK(peakOf(played) > 0.05);
    }

    // A tone above the chip's Nyquist (16 kHz) does not fold back into the audible range, while one below it
    // gets through. At 96 kHz, 28 kHz would land on 4 kHz without the low-pass filter.
    const auto peakOfTone = [&](double frequency)
    {
        const auto file = folder.dir.getChildFile("tone_" + juce::String(static_cast<int>(frequency)) + ".wav");
        writeMono(file, sineWave(frequency, 96000.0, 0.4), 96000.0, 24);
        RetroChipProcessor processor;
        useUserSample(processor);
        CHECK(processor.importSample(file).isEmpty());
        return peakOf(playUserNote(processor, 69, 16), 2 * kBlock);
    };
    CHECK(peakOfTone(6000.0) > 0.05);
    CHECK(peakOfTone(28000.0) < 0.002);
    CHECK(peakOfTone(44000.0) < 0.002);
}

void import_stereo_is_mixed_to_mono()
{
    TempFolder folder;
    const auto tone = sineWave(440.0, 32000.0, 0.5);
    std::vector<float> opposite(tone.size());
    std::vector<float> quiet(tone.size(), 0.0f);
    for (size_t i = 0; i < tone.size(); ++i)
        opposite[i] = -tone[i];

    const auto peakFor = [&](const char* name, const std::vector<std::vector<float>>& channels)
    {
        const auto file = folder.dir.getChildFile(name);
        writeWav(file, channels, 32000.0, 16);
        RetroChipProcessor processor;
        useUserSample(processor);
        CHECK(processor.importSample(file).isEmpty());
        return peakOf(playUserNote(processor, 69, 20), 2 * kBlock);
    };

    const double mono = peakFor("mono.wav", { tone });
    CHECK(mono > 0.05);
    CHECK(std::abs(peakFor("same.wav", { tone, tone }) - mono) < 0.05 * mono);
    CHECK(std::abs(peakFor("half.wav", { tone, quiet }) - 0.5 * mono) < 0.08 * mono);
    CHECK(peakFor("cancel.wav", { tone, opposite }) < 0.01 * mono);
}

void import_various_formats()
{
    TempFolder folder;
    struct Case
    {
        const char* name;
        std::vector<float> samples;
        double rate;
        int bits;
        bool silent;
        size_t blocks;        // expected BRR blocks, 0 to skip
        bool quiet = false;   // too short to hear, so only checked for being kept
    };
    const Case cases[] = {
        { "eight_bit.wav", sineWave(440.0, 32000.0, 0.3), 32000.0, 8, false, 600 },
        { "twenty_four.wav", sineWave(440.0, 32000.0, 0.3), 32000.0, 24, false, 600 },
        { "ninety_six.wav", sineWave(440.0, 96000.0, 0.3), 96000.0, 24, false, 600 },
        { "thirty_two.wav", sineWave(440.0, 44100.0, 0.3), 44100.0, 32, false, 0 },
        { "tiny.wav", sineWave(8000.0, 32000.0, 10.0 / 32000.0), 32000.0, 16, false, 1 }, // 10 samples
        { "one.wav", std::vector<float>(1, 0.5f), 32000.0, 16, false, 1, true },
        { "silent.wav", std::vector<float>(4000, 0.0f), 32000.0, 16, true, 250 },
    };

    for (const auto& c : cases)
    {
        const auto file = folder.dir.getChildFile(c.name);
        writeMono(file, c.samples, c.rate, c.bits);
        const auto result = retrochip::importWav(file);
        CHECK(result.sample != nullptr);
        CHECK(result.message.isEmpty());
        CHECK(!result.truncated);
        if (result.sample == nullptr)
            continue;
        CHECK(result.sample->brr.size() % 9 == 0);
        if (c.blocks != 0 && std::llabs(static_cast<long long>(result.sample->brr.size() / 9) - static_cast<long long>(c.blocks)) > 1)
        {
            std::printf("  FAILED: %s has %zu blocks, expected %zu\n", c.name, result.sample->brr.size() / 9, c.blocks);
            ++failures;
        }
        CHECK(result.sample->loopBlock == 0);
        CHECK(result.sample->name == juce::File(file).getFileNameWithoutExtension().toStdString());

        RetroChipProcessor processor;
        useUserSample(processor);
        CHECK(processor.importSample(file).isEmpty());
        CHECK(processor.getUserSampleName() == file.getFileNameWithoutExtension());
        const double peak = peakOf(playUserNote(processor, 69, 8));
        if (c.silent)
            CHECK(peak == 0.0);
        else
            CHECK(c.quiet || peak > (c.blocks == 1 ? 0.002 : 0.01));
    }

    // A 7-bit-wide quantisation is audible but the pitch holds: the 8-bit file is still the same note.
    RetroChipProcessor processor;
    useUserSample(processor);
    processor.importSample(folder.dir.getChildFile("eight_bit.wav"));
    CHECK(std::abs(estimateFrequency(playUserNote(processor, 69, 16), kSampleRate, 2 * kBlock) - 440.0) < 4.4);
}

void import_peak_is_limited_without_normalising()
{
    TempFolder folder;
    const auto loud = folder.dir.getChildFile("loud.wav");
    const auto quiet = folder.dir.getChildFile("quiet.wav");
    writeMono(loud, sineWave(440.0, 32000.0, 0.3, 3.0f), 32000.0, 32);   // floats above full scale
    writeMono(quiet, sineWave(440.0, 32000.0, 0.3, 0.1f), 32000.0, 16);

    const auto peakWord = [](const std::unique_ptr<retrochip::SnesSample>& sample)
    {
        std::int16_t window[16];
        int old = 0, older = 0, peak = 0;
        for (size_t b = 0; b + 9 <= sample->brr.size(); b += 9)
        {
            retrochip::brr::decodeBlock(sample->brr.data() + b, old, older, window);
            for (const auto v : window)
                peak = std::max(peak, std::abs(static_cast<int>(v)));
        }
        return peak;
    };

    const auto loudResult = retrochip::importWav(loud);
    const auto quietResult = retrochip::importWav(quiet);
    CHECK(loudResult.sample != nullptr && quietResult.sample != nullptr);
    if (loudResult.sample == nullptr || quietResult.sample == nullptr)
        return;
    const int loudPeak = peakWord(loudResult.sample);
    const int quietPeak = peakWord(quietResult.sample);
    CHECK(loudPeak > 0x3000 && loudPeak <= 0x3FFA); // brought down to the range, not clipped flat
    CHECK(quietPeak > 0.08 * 0x3FF8 && quietPeak < 0.12 * 0x3FF8); // left as it was
}

// The file reaches the sample whole. The fade at the end of a sample that does not loop is the engine's,
// tested there, since a looped one must not have it.
void import_keeps_the_end_of_the_sample()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("dc.wav");
    writeMono(file, std::vector<float>(8000, 0.6f));
    const auto result = retrochip::importWav(file);
    CHECK(result.sample != nullptr);
    if (result.sample == nullptr)
        return;

    std::vector<int> decoded;
    std::int16_t window[16];
    int old = 0, older = 0;
    for (size_t b = 0; b + 9 <= result.sample->brr.size(); b += 9)
    {
        retrochip::brr::decodeBlock(result.sample->brr.data() + b, old, older, window);
        decoded.insert(decoded.end(), window, window + 16);
    }
    const size_t n = decoded.size();
    CHECK(n == 8000);
    CHECK(std::abs(decoded[n / 2]) > 0x2000);
    CHECK(std::abs(decoded[n - 161]) > 0x2000);
    CHECK(std::abs(decoded[n - 1]) > 0x2000);
}

// A whole number of cycles loops without a seam: the level holds through every loop point.
void import_looped_sine_holds_its_level()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("cycles.wav");
    writeMono(file, sineWave(440.0, 32000.0, 0.5)); // 220 cycles

    RetroChipProcessor processor;
    useUserSample(processor);
    setParam(processor, retrochip::ids::snesLoop, 1.0f);
    CHECK(processor.importSample(file).isEmpty());
    const auto played = playUserNote(processor, 69, 220); // past four loop points

    constexpr size_t kCycle = 120; // a little over one cycle at the host's rate
    double lowest = 1.0, highest = 0.0;
    for (size_t at = 4 * kBlock; at + kCycle <= played.size(); at += kCycle / 4)
    {
        const double level = peakOf(played, at, at + kCycle);
        lowest = std::min(lowest, level);
        highest = std::max(highest, level);
    }
    CHECK(highest > 0.05);
    if (!(lowest > 0.9 * highest))
    {
        std::printf("  FAILED: a looped sine dips to %g from %g\n", lowest, highest);
        ++failures;
    }
}

// Files of any length and rate fill whole blocks, so a loop closes without padding or a dip at the seam.
void import_loops_close_at_any_length()
{
    struct Case
    {
        double rate;
        int frames;
    };

    TempFolder folder;
    for (const auto test : { Case { 32000.0, 16001 }, Case { 32000.0, 16008 }, Case { 44100.0, 22051 }, Case { 48000.0, 24013 } })
    {
        std::vector<float> wave(static_cast<size_t>(test.frames));
        for (size_t i = 0; i < wave.size(); ++i)
            wave[i] = 0.5f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * static_cast<double>(i) / test.frames));

        const auto file = folder.dir.getChildFile("loop.wav");
        writeMono(file, wave, test.rate);
        const auto result = retrochip::importWav(file);
        CHECK(result.sample != nullptr);
        if (result.sample == nullptr)
            continue;

        std::vector<int> decoded;
        std::int16_t window[16];
        int old = 0, older = 0;
        for (size_t b = 0; b + 9 <= result.sample->brr.size(); b += 9)
        {
            retrochip::brr::decodeBlock(result.sample->brr.data() + b, old, older, window);
            decoded.insert(decoded.end(), window, window + 16);
        }

        const size_t n = decoded.size();
        const double expected = test.frames * 32000.0 / test.rate;
        CHECK(std::abs(static_cast<double>(n) - expected) <= 8.0);

        int largestStep = 0, largestLevel = 0, lastLevel = 0, firstLevel = 0;
        for (size_t i = 0; i < n; ++i)
        {
            largestLevel = std::max(largestLevel, std::abs(decoded[i]));
            if (i > 0)
                largestStep = std::max(largestStep, std::abs(decoded[i] - decoded[i - 1]));
            if (i < 80)
                firstLevel = std::max(firstLevel, std::abs(decoded[i]));
            if (i + 80 >= n)
                lastLevel = std::max(lastLevel, std::abs(decoded[i]));
        }
        const int seamStep = std::abs(decoded.front() - decoded.back());

        // The wave stays whole right up to both ends, where a filter fed with silence would pull it down.
        double worstEdgeError = 0.0;
        for (size_t i = 0; i < n; i = (i == 23 ? n - 24 : i + 1))
        {
            const double ideal = largestLevel * std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * static_cast<double>(i) / static_cast<double>(n));
            worstEdgeError = std::max(worstEdgeError, std::abs(decoded[i] - ideal));
        }

        if (seamStep > 1.5 * largestStep || firstLevel < 0.9 * largestLevel || lastLevel < 0.9 * largestLevel
            || worstEdgeError > 0.1 * largestLevel)
        {
            std::printf("  FAILED: %g Hz, %d frames: seam %d against step %d, ends %d and %d against level %d, edge error %g\n",
                        test.rate, test.frames, seamStep, largestStep, firstLevel, lastLevel, largestLevel, worstEdgeError);
            ++failures;
        }
    }
}

void import_invalid_keeps_current()
{
    TempFolder folder;
    const auto good = folder.dir.getChildFile("good.wav");
    writeMono(good, sineWave(440.0, 32000.0, 0.5));
    const auto text = folder.dir.getChildFile("notes.txt");
    text.replaceWithText("this is not a sound");
    const auto emptyFile = folder.dir.getChildFile("empty.wav");
    CHECK(emptyFile.create().wasOk());
    const auto missing = folder.dir.getChildFile("missing.wav");
    const auto noFrames = folder.dir.getChildFile("no_frames.wav");
    writeWav(noFrames, { {} }, 32000.0, 16);

    RetroChipProcessor processor;
    useUserSample(processor);
    CHECK(processor.importSample(good).isEmpty());
    const auto before = playUserNote(processor, 69, 10);
    CHECK(peakOf(before) > 0.05);

    const auto expectMessage = [&](const juce::File& file, const char* word)
    {
        const auto message = processor.importSample(file);
        CHECK(message.isNotEmpty());
        CHECK(message.contains(file.getFileName()));
        CHECK(message.containsIgnoreCase(word));
        CHECK(processor.getSampleStatus() == message);
        CHECK(processor.getUserSampleName() == "good");
        CHECK(maxDifference(before, playUserNote(processor, 69, 10)) == 0.0);
    };
    expectMessage(text, "unreadable");
    expectMessage(missing, "unreadable");
    expectMessage(emptyFile, "empty");
    expectMessage(noFrames, "empty");

    // A file that exists but cannot be opened is unreadable too, and not a crash.
    if (kPosixFiles)
    {
        const auto locked = folder.dir.getChildFile("locked.wav");
        CHECK(good.copyFileTo(locked));
        CHECK(setMode(locked, 0));
        expectMessage(locked, "unreadable");
        CHECK(setMode(locked, 0644)); // so that the folder can be removed
    }

    // A good file afterwards clears the message.
    CHECK(processor.importSample(good).isEmpty());
    CHECK(processor.getSampleStatus() == "good");

    // With nothing loaded before, a failure leaves the instrument silent and the prompt away.
    RetroChipProcessor fresh;
    useUserSample(fresh);
    CHECK(fresh.importSample(text).isNotEmpty());
    CHECK(fresh.getUserSampleName().isEmpty());
    CHECK(peakOf(playUserNote(fresh, 69, 6)) == 0.0);
}

// A PCM WAV with the header fields as given, and silence for data.
void writeRawWav(const juce::File& file, unsigned channels, juce::uint32 rate, unsigned frames)
{
    juce::MemoryOutputStream out;
    const auto blockAlign = static_cast<juce::uint16>(channels * 2);
    const juce::uint32 dataSize = frames * blockAlign;
    out.write("RIFF", 4);
    out.writeInt(static_cast<int>(36 + dataSize));
    out.write("WAVEfmt ", 8);
    out.writeInt(16);
    out.writeShort(1);
    out.writeShort(static_cast<short>(channels));
    out.writeInt(static_cast<int>(rate));
    out.writeInt(static_cast<int>(rate * blockAlign));
    out.writeShort(static_cast<short>(blockAlign));
    out.writeShort(16);
    out.write("data", 4);
    out.writeInt(static_cast<int>(dataSize));
    for (juce::uint32 i = 0; i < dataSize; ++i)
        out.writeByte(0);
    file.replaceWithData(out.getData(), out.getDataSize());
}

void import_rejects_implausible_headers()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("header.wav");

    // The helper makes files the importer accepts, so the rejections below are about the header fields.
    writeRawWav(file, 1, 32000, 400);
    CHECK(retrochip::importWav(file).sample != nullptr);
    writeRawWav(file, 2, 8000, 400);
    CHECK(retrochip::importWav(file).sample != nullptr);
    writeRawWav(file, 64, 32000, 40);
    CHECK(retrochip::importWav(file).sample != nullptr);

    const struct { unsigned channels; juce::uint32 rate; } bad[] = {
        { 65, 32000 }, { 200, 32000 }, { 4000, 32000 }, { 65535, 32000 },
        { 1, 500 }, { 1, 999 }, { 1, 768001 }, { 1, 2000000 }, { 1, 4000000000u }, { 1, 1 }
    };
    for (const auto& b : bad)
    {
        writeRawWav(file, b.channels, b.rate, 4);
        const auto result = retrochip::importWav(file);
        if (result.sample != nullptr || !result.message.containsIgnoreCase("unreadable"))
            std::printf("  FAILED case: %u channels at %u Hz\n", b.channels, static_cast<unsigned>(b.rate));
        CHECK(result.sample == nullptr);
        CHECK(result.message.containsIgnoreCase("unreadable"));
    }

    // The same through the plugin: the message is set and the current sample stays.
    RetroChipProcessor processor;
    useUserSample(processor);
    writeMono(folder.dir.getChildFile("good.wav"), sineWave(440.0, 32000.0, 0.2));
    CHECK(processor.importSample(folder.dir.getChildFile("good.wav")).isEmpty());
    writeRawWav(file, 65535, 4000000000u, 4);
    CHECK(processor.importSample(file).containsIgnoreCase("unreadable"));
    CHECK(processor.getUserSampleName() == "good");
}

void import_too_long_is_truncated()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("long.wav");
    writeMono(file, sineWave(220.0, 32000.0, 10.0));

    const auto result = retrochip::importWav(file);
    CHECK(result.sample != nullptr);
    CHECK(result.truncated);
    CHECK(result.message.contains("64 KB"));
    CHECK(result.message.contains(juce::String::fromUTF8("3.6\xC2\xA0s")));
    CHECK(result.message.startsWith(juce::String("Sample cut")));
    if (result.sample != nullptr)
    {
        CHECK(result.sample->brr.size() <= 65529);
        CHECK(result.sample->brr.size() == 65529);
        CHECK((result.sample->brr[result.sample->brr.size() - 9] & 1) != 0);
    }

    // The limit is exact: 7281 blocks fit, one sample more does not.
    const auto edge = folder.dir.getChildFile("edge.wav");
    writeMono(edge, sineWave(220.0, 32000.0, 116496.0 / 32000.0));
    const auto fits = retrochip::importWav(edge);
    CHECK(fits.sample != nullptr && !fits.truncated && fits.message.isEmpty());
    writeMono(edge, sineWave(220.0, 32000.0, 116497.0 / 32000.0));
    const auto over = retrochip::importWav(edge);
    CHECK(over.sample != nullptr && over.truncated);

    // Through the plugin: the sample is kept, and the status says what happened.
    RetroChipProcessor processor;
    useUserSample(processor);
    const auto message = processor.importSample(file);
    CHECK(message == result.message);
    CHECK(processor.getSampleStatus() == message);
    CHECK(processor.getUserSampleName() == "long");
    CHECK(peakOf(playUserNote(processor, 57, 10)) > 0.05);
}

void user_sample_round_trips_in_state()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("my voice.wav");
    writeMono(file, sineWave(440.0, 48000.0, 0.4), 48000.0);

    juce::MemoryBlock saved;
    std::vector<float> expected;
    {
        RetroChipProcessor processor;
        useUserSample(processor);
        setParam(processor, retrochip::ids::snesRootNote, 64.0f);
        setParam(processor, retrochip::ids::snesLoop, 1.0f);
        setParam(processor, retrochip::ids::snesAttack, 11.0f);
        CHECK(processor.importSample(file).isEmpty());
        processor.getStateInformation(saved);
        expected = playUserNote(processor, 64, 30);
    }

    RetroChipProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    CHECK(restored.getUserSampleName() == "my voice");
    CHECK(restored.getSampleStatus() == "my voice");
    CHECK(getParam(restored, retrochip::ids::snesInstrument) == 12.0f);
    CHECK(getParam(restored, retrochip::ids::snesRootNote) == 64.0f);
    CHECK(getParam(restored, retrochip::ids::snesLoop) == 1.0f);
    CHECK(getParam(restored, retrochip::ids::snesAttack) == 11.0f);
    const auto again = playUserNote(restored, 64, 30);
    CHECK(peakOf(again) > 0.05);
    CHECK(maxDifference(expected, again) < 1e-6);

    // A second save of the restored plugin is the same sample (and keeps looping at the end).
    juce::MemoryBlock resaved;
    restored.getStateInformation(resaved);
    RetroChipProcessor third;
    third.setStateInformation(resaved.getData(), static_cast<int>(resaved.getSize()));
    CHECK(maxDifference(expected, playUserNote(third, 64, 30)) < 1e-6);

    // Loading a state with no sample replaces the one the plugin had.
    RetroChipProcessor none;
    useUserSample(none);
    juce::MemoryBlock withoutSample;
    none.getStateInformation(withoutSample);
    CHECK(none.getUserSampleName().isEmpty());
    restored.setStateInformation(withoutSample.getData(), static_cast<int>(withoutSample.getSize()));
    CHECK(restored.getUserSampleName().isEmpty());
    CHECK(restored.getSampleStatus() == kImportPrompt); // the state chose the user's sample
    CHECK(peakOf(playUserNote(restored, 64, 6)) == 0.0);
}

// The state with the sample's BRR (and optionally its name) replaced.
juce::MemoryBlock withSampleProperty(const juce::MemoryBlock& state, const juce::String& value)
{
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    CHECK(xml != nullptr);
    juce::MemoryBlock out;
    if (xml == nullptr)
        return out;
    CHECK(xml->hasAttribute(retrochip::kSampleProperty));
    xml->setAttribute(retrochip::kSampleProperty, value);
    juce::AudioProcessor::copyXmlToBinary(*xml, out);
    return out;
}

void corrupt_sample_in_state_keeps_rest()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("good.wav");
    writeMono(file, sineWave(440.0, 32000.0, 0.3));

    juce::MemoryBlock good;
    {
        RetroChipProcessor processor;
        useUserSample(processor);
        setParam(processor, retrochip::ids::snesAttack, 10.0f);
        setParam(processor, retrochip::ids::snesRelease, 640.0f);
        setParam(processor, retrochip::ids::snesLoop, 1.0f);
        processor.importSample(file);
        processor.getStateInformation(good);
    }

    const auto base64Of = [](size_t bytes)
    {
        std::vector<std::uint8_t> data(bytes, 0);
        return juce::Base64::toBase64(data.data(), data.size());
    };

    const std::pair<const char*, juce::String> broken[] = {
        { "not base64", "!!!this is not base64!!!" },
        { "ten bytes", base64Of(10) },
        { "one byte short of a block", base64Of(80) },
        { "over 64 KB", base64Of(9 * 7282) },
        { "way over", base64Of(200000) },
    };

    for (const auto& [what, value] : broken)
    {
        const auto state = withSampleProperty(good, value);

        RetroChipProcessor processor;
        useUserSample(processor);
        processor.importSample(file); // something to be replaced
        processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

        if (getParam(processor, retrochip::ids::snesAttack) != 10.0f)
            std::printf("  FAILED case: %s\n", what);
        CHECK(getParam(processor, retrochip::ids::snesAttack) == 10.0f);
        CHECK(std::abs(getParam(processor, retrochip::ids::snesRelease) - 640.0f) < 1.5f);
        CHECK(getParam(processor, retrochip::ids::snesLoop) == 1.0f);
        CHECK(getParam(processor, retrochip::ids::snesInstrument) == 12.0f);
        CHECK(processor.getUserSampleName().isEmpty());
        CHECK(processor.getSampleStatus().isNotEmpty());
        CHECK(processor.getSampleStatus().containsIgnoreCase("unreadable"));
        CHECK(processor.getSampleStatus() != kImportPrompt);
        CHECK(peakOf(playUserNote(processor, 69, 8)) == 0.0);
    }

    // The exact limit is accepted.
    const auto atLimit = withSampleProperty(good, base64Of(9 * 7281));
    RetroChipProcessor processor;
    processor.setStateInformation(atLimit.getData(), static_cast<int>(atLimit.getSize()));
    CHECK(processor.getSampleStatus() != kImportPrompt);
    CHECK(!processor.getSampleStatus().containsIgnoreCase("unreadable"));
}

void no_sample_prompts_to_import()
{
    RetroChipProcessor processor;
    CHECK(processor.getUserSampleName().isEmpty());

    // A bank instrument plays as it is, so there is nothing to invite.
    CHECK(processor.getSampleStatus().isEmpty());
    useUserSample(processor);
    CHECK(peakOf(playUserNote(processor, 69, 6)) == 0.0);
    CHECK(processor.getSampleStatus() == kImportPrompt);
    setParam(processor, retrochip::ids::snesInstrument, 3.0f);
    CHECK(processor.getSampleStatus().isEmpty());

    // A sample's name and a message stay, whatever the instrument.
    TempFolder folder;
    const auto file = folder.dir.getChildFile("tone.wav");
    writeMono(file, sineWave(440.0, 32000.0, 0.1));
    CHECK(processor.importSample(file).isEmpty());
    CHECK(processor.getSampleStatus() == "tone");
    setParam(processor, retrochip::ids::snesInstrument, 12.0f);
    CHECK(processor.getSampleStatus() == "tone");
}

void replacing_sample_during_play_is_safe()
{
    TempFolder folder;
    const auto first = folder.dir.getChildFile("first.wav");
    const auto second = folder.dir.getChildFile("second.wav");
    writeMono(first, sineWave(440.0, 32000.0, 0.5));
    writeMono(second, sineWave(880.0, 32000.0, 0.5));

    RetroChipProcessor processor;
    useUserSample(processor);
    auto* snes = dynamic_cast<SnesEngine*>(processor.engineFor(Console::SDsp));
    CHECK(snes != nullptr);
    if (snes == nullptr)
        return;

    // Imports with no audio running replace each other without piling up.
    for (int i = 0; i < 6; ++i)
    {
        CHECK(processor.importSample(i % 2 == 0 ? first : second).isEmpty());
        CHECK(processor.getRetiredSampleCount() == 0);
    }
    CHECK(processor.getUserSampleName() == "first" || processor.getUserSampleName() == "second");
    CHECK(processor.importSample(first).isEmpty());

    // A note is playing the first sample.
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(run(processor, 4, 69) > 0.05);
    CHECK(snes->activeVoices() == 1);

    // The new sample is published between blocks. The old one stays until the audio thread has moved on.
    CHECK(processor.importSample(second).isEmpty());
    CHECK(processor.getRetiredSampleCount() == 1);
    processor.releaseRetiredSamples();
    CHECK(processor.getRetiredSampleCount() == 1);
    CHECK(processor.importSample(first).isEmpty()); // not taken yet: the one in between is simply dropped
    CHECK(processor.getRetiredSampleCount() == 1);
    CHECK(processor.importSample(second).isEmpty());
    CHECK(processor.getRetiredSampleCount() == 1);
    CHECK(snes->activeVoices() == 1); // the engine has not been told yet

    run(processor, 1); // the next block adopts it and stops the old voices
    CHECK(snes->activeVoices() == 0);
    processor.releaseRetiredSamples();
    CHECK(processor.getRetiredSampleCount() == 0);

    // The new sample plays.
    const auto replayed = playUserNote(processor, 69, 20);
    CHECK(std::abs(estimateFrequency(replayed, kSampleRate, 2 * kBlock) - 880.0) < 8.8);

    // Imports in between blocks, with notes held, many times over.
    juce::AudioBuffer<float> buffer(2, kBlock);
    for (int i = 0; i < 40; ++i)
    {
        juce::MidiBuffer midi;
        if (i % 3 == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60 + i % 12, static_cast<juce::uint8>(100)), 5);
        processor.processBlock(buffer, midi);
        for (int s = 0; s < kBlock; ++s)
            CHECK(std::isfinite(buffer.getSample(0, s)));
        CHECK(processor.importSample(i % 2 == 0 ? first : second).isEmpty());
        if (i % 5 == 0)
            CHECK(processor.importSample(i % 2 == 0 ? second : first).isEmpty());
        CHECK(processor.getRetiredSampleCount() <= 1);
        processor.releaseRetiredSamples();
    }
    run(processor, 1);
    processor.releaseRetiredSamples();
    CHECK(processor.getRetiredSampleCount() == 0);
    CHECK(peakOf(playUserNote(processor, 69, 8)) > 0.05);

    // Loading a state while notes sound is the same thing.
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    CHECK(run(processor, 2, 69) > 0.05);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(processor.getRetiredSampleCount() == 1);
    run(processor, 1);
    CHECK(snes->activeVoices() == 0);
    processor.releaseRetiredSamples();
    CHECK(processor.getRetiredSampleCount() == 0);
}

// The same with a real audio thread, which is the case the atomics are for.
void replacing_sample_while_audio_runs()
{
    TempFolder folder;
    const auto first = folder.dir.getChildFile("first.wav");
    const auto second = folder.dir.getChildFile("second.wav");
    writeMono(first, sineWave(440.0, 32000.0, 0.1));
    writeMono(second, sineWave(660.0, 44100.0, 0.15), 44100.0);

    RetroChipProcessor processor;
    useUserSample(processor);
    juce::MemoryBlock emptyState; // the same settings with no sample, as a host that loads one would send
    processor.getStateInformation(emptyState);
    CHECK(processor.importSample(first).isEmpty());
    processor.prepareToPlay(kSampleRate, kBlock);

    std::atomic<bool> stop { false };
    std::atomic<int> blocks { 0 };
    std::atomic<bool> finite { true };
    std::thread audio([&]
    {
        juce::AudioBuffer<float> buffer(2, kBlock);
        for (int i = 0; !stop.load(); ++i)
        {
            juce::MidiBuffer midi;
            if (i % 2 == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 60 + i % 24, static_cast<juce::uint8>(100)), i % kBlock);
            processor.processBlock(buffer, midi);
            for (int s = 0; s < kBlock; ++s)
                if (!std::isfinite(buffer.getSample(0, s)))
                    finite = false;
            ++blocks;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    // Imports alternate with loads of a state with no sample, which publish a null: the case where a stale
    // acknowledgement of an earlier null could free a sample the audio thread was about to adopt.
    for (int i = 0; i < 400; ++i)
    {
        CHECK(processor.importSample(i % 2 == 0 ? second : first).isEmpty());
        if (i % 3 != 2)
            processor.setStateInformation(emptyState.getData(), static_cast<int>(emptyState.getSize()));
        if (i % 7 == 0)
            CHECK(processor.importSample(first).isEmpty());
        processor.releaseRetiredSamples();
        CHECK(processor.getRetiredSampleCount() <= 2);
        if (i % 20 == 0)
        {
            juce::MemoryBlock state;
            processor.getStateInformation(state);
            processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        }
        std::this_thread::sleep_for(std::chrono::microseconds(i % 5 == 0 ? 1000 : 50 * (i % 11)));
    }
    stop = true;
    audio.join();

    CHECK(blocks.load() > 20);
    CHECK(finite.load());
    run(processor, 1);
    processor.releaseRetiredSamples();
    CHECK(processor.getRetiredSampleCount() == 0);
}

void loop_setting_applies_to_user_sample()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("short.wav");
    writeMono(file, sineWave(440.0, 32000.0, 0.2)); // 0.2 s, 19 blocks at the host's rate

    const size_t tail = 30 * kBlock;
    const auto render = [&](bool loop)
    {
        RetroChipProcessor processor;
        useUserSample(processor);
        setParam(processor, retrochip::ids::snesLoop, loop ? 1.0f : 0.0f);
        CHECK(processor.importSample(file).isEmpty());
        return playUserNote(processor, 69, 40);
    };

    const auto once = render(false);
    CHECK(peakOf(once, 0, 10 * kBlock) > 0.05);

    CHECK(peakOf(once, tail) < 1e-3);

    const auto looped = render(true);
    CHECK(peakOf(looped, tail) > 0.05);
    CHECK(std::abs(estimateFrequency(looped, kSampleRate, tail) - 440.0) < 10.0);

    // The switch acts live, on a note already sounding.
    RetroChipProcessor processor;
    useUserSample(processor);
    setParam(processor, retrochip::ids::snesLoop, 1.0f);
    CHECK(processor.importSample(file).isEmpty());
    playUserNote(processor, 69, 10);
    CHECK(peakOf(processWith(processor, 10)) > 0.05);
    setParam(processor, retrochip::ids::snesLoop, 0.0f);
    processWith(processor, 25);
    CHECK(peakOf(processWith(processor, 6)) < 1e-3);
}

void editor_import_panel_shows_the_status()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("tone.wav");
    writeMono(file, sineWave(440.0, 32000.0, 0.3));
    const auto text = folder.dir.getChildFile("bad.txt");
    text.replaceWithText("x");

    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    setParam(processor, kConsoleId, 2.0f);

    auto* button = dynamic_cast<juce::TextButton*>(findById(*editor, "snesImport"));
    auto* label = dynamic_cast<juce::Label*>(findById(*editor, "snesSampleStatus"));
    CHECK(button != nullptr && label != nullptr);
    if (button == nullptr || label == nullptr)
        return;

    CHECK(button->isVisible() && button->isEnabled());
    CHECK(button->getButtonText() == "Import");
    CHECK(button->getTitle().isNotEmpty());
    CHECK(label->isVisible());

    // The invitation comes and goes with the instrument.
    CHECK(label->getText().isEmpty());
    setParam(processor, retrochip::ids::snesInstrument, 12.0f);
    CHECK(label->getText() == kImportPrompt);
    CHECK(label->getTooltip() == kImportPrompt);
    setParam(processor, retrochip::ids::snesInstrument, 0.0f);
    CHECK(label->getText().isEmpty());
    setParam(processor, retrochip::ids::snesInstrument, 12.0f);
    CHECK(label->getText() == kImportPrompt);
    checkTitles(*editor);
    checkLayout(*editor);

    CHECK(processor.importSample(file).isEmpty());
    CHECK(label->getText() == "tone");

    CHECK(processor.importSample(text).isNotEmpty());
    CHECK(label->getText() == processor.getSampleStatus());
    CHECK(label->getText().containsIgnoreCase("unreadable"));
    checkLayout(*editor);

    // A long message wraps inside its panel, and its number and unit stay together.
    writeMono(folder.dir.getChildFile("long.wav"), sineWave(220.0, 32000.0, 5.0));
    CHECK(processor.importSample(folder.dir.getChildFile("long.wav")).isNotEmpty());
    CHECK(label->getText().contains("64 KB"));
    CHECK(label->getText().contains(juce::String::fromUTF8("3.6\xC2\xA0s")));
    CHECK(!label->getText().contains("3,6 s"));
    checkLayout(*editor);
    CHECK(label->getRight() <= button->getParentComponent()->getWidth());
    setParam(processor, retrochip::ids::snesInstrument, 12.0f);
    saveSnapshot(*editor, "editor_sdsp_import.png");

    setParam(processor, kConsoleId, 0.0f);
    CHECK(!button->isVisible());
    CHECK(!label->isVisible());
}

// The editor's import path, which the file chooser calls: the user's sample becomes the instrument, but only
// on success, and neither importSample nor a state load does it.
void editor_import_selects_the_user_sample()
{
    TempFolder folder;
    const auto good = folder.dir.getChildFile("good.wav");
    const auto bad = folder.dir.getChildFile("bad.txt");
    writeMono(good, sineWave(440.0, 32000.0, 0.3));
    bad.replaceWithText("x");
    writeMono(folder.dir.getChildFile("long.wav"), sineWave(220.0, 32000.0, 5.0));

    RetroChipProcessor processor;
    selectSdsp(processor);
    setParam(processor, retrochip::ids::snesInstrument, 3.0f);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto* concrete = dynamic_cast<retrochip::RetroChipEditor*>(editor.get());
    CHECK(concrete != nullptr);
    if (concrete == nullptr)
        return;

    // The processor's import leaves the instrument alone.
    bool imported = false;
    CHECK(processor.importSample(good, &imported).isEmpty());
    CHECK(imported);
    CHECK(getParam(processor, retrochip::ids::snesInstrument) == 3.0f);

    // A failure from the editor changes nothing.
    concrete->importFile(bad);
    CHECK(getParam(processor, retrochip::ids::snesInstrument) == 3.0f);
    CHECK(processor.getSampleStatus().containsIgnoreCase("unreadable"));

    // A success selects the sample, which then plays.
    concrete->importFile(good);
    CHECK(getParam(processor, retrochip::ids::snesInstrument) == 12.0f);
    auto* box = dynamic_cast<juce::ComboBox*>(findById(*editor, retrochip::ids::snesInstrument));
    CHECK(box != nullptr && box->getSelectedItemIndex() == 12);
    setParam(processor, retrochip::ids::snesAttack, 15.0f);
    setParam(processor, retrochip::ids::snesEchoMix, 0.0f);
    CHECK(peakOf(playUserNote(processor, 60, 8)) > 0.02);

    // A cut file is still an import. Then a failure leaves the choice where it was.
    setParam(processor, retrochip::ids::snesInstrument, 5.0f);
    concrete->importFile(folder.dir.getChildFile("long.wav"));
    CHECK(getParam(processor, retrochip::ids::snesInstrument) == 12.0f);
    setParam(processor, retrochip::ids::snesInstrument, 5.0f);
    concrete->importFile(bad);
    CHECK(getParam(processor, retrochip::ids::snesInstrument) == 5.0f);

    // Loading a state never changes the instrument either.
    juce::MemoryBlock state;
    {
        RetroChipProcessor other;
        selectSdsp(other);
        setParam(other, retrochip::ids::snesInstrument, 7.0f);
        other.importSample(good);
        other.getStateInformation(state);
    }
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(getParam(processor, retrochip::ids::snesInstrument) == 7.0f);
    CHECK(processor.getUserSampleName() == "good");
}

void sdsp_snapshot()
{
    RetroChipProcessor processor;
    processor.setCurrentProgram(28);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    CHECK(editor->getWidth() == 760);
    CHECK(editor->getHeight() == 520);
    checkTitles(*editor);
    checkLayout(*editor);
    saveSnapshot(*editor, "editor_sdsp.png");

    // The user's sample is live, and the widest values: the longest texts each knob can show.
    setParam(processor, retrochip::ids::snesInstrument, 12.0f);
    setParam(processor, retrochip::ids::snesAttack, 0.0f);
    setParam(processor, retrochip::ids::snesSustainRate, 17.0f);
    setParam(processor, retrochip::ids::snesRelease, 2000.0f);
    setParam(processor, retrochip::ids::snesEchoDelay, 15.0f);
    setParam(processor, retrochip::ids::snesEchoMix, 100.0f);
    setParam(processor, retrochip::ids::vibratoRate, 12.0f);
    setParam(processor, retrochip::ids::vibratoDepth, 100.0f);
    setParam(processor, retrochip::ids::glide, 1000.0f);
    setParam(processor, retrochip::ids::gain, -36.0f);
    checkLayout(*editor);
    saveSnapshot(*editor, "editor_sdsp_sample.png");
}

// ---- Moving between the consoles ----

constexpr int kNumConsoles = 3;
constexpr int kProgramOfConsole[kNumConsoles] = { 0, 12, 24 };

juce::MidiBuffer noteOnAtStart(int note)
{
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(120)), 0);
    return midi;
}

// A note that is on and released within the first block, so only the pedal can hold it.
juce::MidiBuffer shortNote(int note)
{
    auto midi = noteOnAtStart(note);
    midi.addEvent(juce::MidiMessage::noteOff(1, note), 100);
    return midi;
}

juce::MidiBuffer pedal(bool down)
{
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, down ? 127 : 0), 0);
    return midi;
}

juce::MidiBuffer wheelUp()
{
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::pitchWheel(1, 16383), 0);
    return midi;
}

bool noteActive(RetroChipProcessor& processor, int console, int note)
{
    auto* engine = processor.engineFor(static_cast<Console>(console));
    if (auto* nes = dynamic_cast<retrochip::NesEngine*>(engine))
        return nes->isNoteActive(note);
    if (auto* genesis = dynamic_cast<GenesisEngine*>(engine))
        return genesis->isNoteActive(note);
    if (auto* snes = dynamic_cast<SnesEngine*>(engine))
        return snes->isNoteActive(note);
    CHECK(false);
    return false;
}

// Long enough for the slowest release to end: the YM2612's is capped at 2 s.
constexpr int kBlocksToEndARelease = 220;

void return_to_a_console_has_no_stale_tail()
{
    for (int console = 0; console < kNumConsoles; ++console)
    {
        RetroChipProcessor processor;
        processor.setCurrentProgram(kProgramOfConsole[console]);
        processor.prepareToPlay(kSampleRate, kBlock);
        CHECK(peakOf(processWith(processor, 6, noteOnAtStart(48))) > 0.02);

        setConsole(processor, (console + 1) % kNumConsoles);
        processWith(processor, 2);
        setConsole(processor, console);

        const double first = peakOf(processWith(processor, 1));
        if (!(first < 1e-3))
        {
            std::printf("  FAILED: console %d comes back with a tail, peak %g\n", console, first);
            ++failures;
        }
    }
}

void first_note_after_a_return_does_not_glide()
{
    constexpr int kFirst = 48;
    constexpr int kAfter = 84;
    for (int console = 0; console < kNumConsoles; ++console)
    {
        const auto ready = [console](RetroChipProcessor& processor)
        {
            processor.setCurrentProgram(kProgramOfConsole[console]);
            setParam(processor, retrochip::ids::glide, 1000.0f);
            setParam(processor, retrochip::ids::vibratoDepth, 0.0f);
            processor.prepareToPlay(kSampleRate, kBlock);
        };

        RetroChipProcessor fresh;
        ready(fresh);
        const double expected = estimateFrequency(processWith(fresh, 10, noteOnAtStart(kAfter)), kSampleRate, kBlock);
        CHECK(expected > 500.0);

        RetroChipProcessor processor;
        ready(processor);
        processWith(processor, 6, noteOnAtStart(kFirst));
        setConsole(processor, (console + 1) % kNumConsoles);
        processWith(processor, 2);
        setConsole(processor, console);
        const double played = estimateFrequency(processWith(processor, 10, noteOnAtStart(kAfter)), kSampleRate, kBlock);
        if (!(std::abs(played - expected) < 0.05 * expected))
        {
            std::printf("  FAILED: console %d glides after a return: %g Hz, not %g Hz\n", console, played, expected);
            ++failures;
        }
    }
}

void pedal_and_bend_follow_into_the_new_console()
{
    for (int from = 0; from < kNumConsoles; ++from)
    {
        for (int to = 0; to < kNumConsoles; ++to)
        {
            if (from == to)
                continue;

            // The pedal stays down across the switch: the note outlasts its key, until the pedal comes up.
            RetroChipProcessor held;
            setConsole(held, from);
            held.prepareToPlay(kSampleRate, kBlock);
            processWith(held, 1, pedal(true));
            setConsole(held, to);
            processWith(held, kBlocksToEndARelease, shortNote(60));
            CHECK(noteActive(held, to, 60));
            processWith(held, kBlocksToEndARelease, pedal(false));
            CHECK(!noteActive(held, to, 60));

            // The wheel stays up across the switch: the note sounds as if it had been moved on this console.
            const auto play = [to](bool bent)
            {
                RetroChipProcessor processor;
                setConsole(processor, to);
                processor.prepareToPlay(kSampleRate, kBlock);
                auto midi = bent ? wheelUp() : juce::MidiBuffer();
                midi.addEvents(noteOnAtStart(60), 0, -1, 0);
                return processWith(processor, 12, midi);
            };
            const auto bent = play(true);
            CHECK(maxDifference(bent, play(false)) > 0.01);

            RetroChipProcessor switched;
            setConsole(switched, from);
            switched.prepareToPlay(kSampleRate, kBlock);
            processWith(switched, 1, wheelUp());
            setConsole(switched, to);
            const auto carried = processWith(switched, 12, noteOnAtStart(60));
            const double largest = maxDifference(bent, carried);
            if (!(largest < 1e-5))
            {
                std::printf("  FAILED: the bend from console %d to %d differs by %g\n", from, to, largest);
                ++failures;
            }
        }
    }
}

// A pedal or a wheel from before prepareToPlay is gone: on the engines, and for a later switch.
void prepare_clears_pedal_and_bend()
{
    juce::MidiBuffer both = pedal(true);
    both.addEvents(wheelUp(), 0, -1, 0);

    for (int console = 0; console < kNumConsoles; ++console)
    {
        const auto freshNote = [](int on)
        {
            RetroChipProcessor processor;
            setConsole(processor, on);
            processor.prepareToPlay(kSampleRate, kBlock);
            return processWith(processor, 12, noteOnAtStart(60));
        };

        // Staying on the console.
        RetroChipProcessor pedalled;
        setConsole(pedalled, console);
        pedalled.prepareToPlay(kSampleRate, kBlock);
        processWith(pedalled, 2, both);
        pedalled.prepareToPlay(kSampleRate, kBlock);
        processWith(pedalled, kBlocksToEndARelease, shortNote(60));
        CHECK(!noteActive(pedalled, console, 60));

        RetroChipProcessor bent;
        setConsole(bent, console);
        bent.prepareToPlay(kSampleRate, kBlock);
        processWith(bent, 2, both);
        bent.prepareToPlay(kSampleRate, kBlock);
        CHECK(maxDifference(freshNote(console), processWith(bent, 12, noteOnAtStart(60))) < 1e-5);

        // Moving to another afterwards.
        const int to = (console + 1) % kNumConsoles;
        RetroChipProcessor movedPedal;
        setConsole(movedPedal, console);
        movedPedal.prepareToPlay(kSampleRate, kBlock);
        processWith(movedPedal, 2, both);
        movedPedal.prepareToPlay(kSampleRate, kBlock);
        setConsole(movedPedal, to);
        processWith(movedPedal, kBlocksToEndARelease, shortNote(60));
        CHECK(!noteActive(movedPedal, to, 60));

        RetroChipProcessor movedBend;
        setConsole(movedBend, console);
        movedBend.prepareToPlay(kSampleRate, kBlock);
        processWith(movedBend, 2, both);
        movedBend.prepareToPlay(kSampleRate, kBlock);
        setConsole(movedBend, to);
        CHECK(maxDifference(freshNote(to), processWith(movedBend, 12, noteOnAtStart(60))) < 1e-5);
    }
}

// A menu pick reaches the processor a moment after the box changes: the timer must not undo it meanwhile.
void preset_menu_timer_leaves_a_pick_in_progress()
{
    const auto flushTimer = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil(250); };

    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto* box = dynamic_cast<juce::ComboBox*>(findById(*editor, "preset"));
    CHECK(box != nullptr);
    if (box == nullptr)
        return;

    box->setSelectedId(5, juce::dontSendNotification);
    flushTimer();
    CHECK(box->getSelectedId() == 5);

    // A change from the host still shows.
    processor.setCurrentProgram(2);
    flushTimer();
    CHECK(box->getSelectedId() == 3);
}

void preset_menu_is_empty_on_another_console()
{
    const auto flushTimer = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil(250); };
    const auto presetBoxOf = [](juce::Component& editor)
    {
        auto* box = dynamic_cast<juce::ComboBox*>(findById(editor, "preset"));
        CHECK(box != nullptr);
        return box;
    };

    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto* box = presetBoxOf(*editor);
    if (box == nullptr)
        return;
    CHECK(box->getSelectedId() == 1);
    CHECK(box->getText() == processor.getProgramName(0));

    // The console changes: the current preset is not this console's, and the menu says nothing.
    setParam(processor, kConsoleId, 2.0f);
    CHECK(box->getSelectedId() == 0);
    CHECK(box->getText().isEmpty());
    flushTimer();
    CHECK(box->getSelectedId() == 0);

    // So the preset can be picked again, which brings back its console.
    box->setSelectedId(1, juce::sendNotificationSync);
    CHECK(processor.getCurrentProgram() == 0);
    CHECK(processor.getActiveConsole() == Console::Chip2A03);
    CHECK(box->getSelectedId() == 1);

    // Picking another console's preset selects it at once, for all that the console changes on the way.
    box->setSelectedId(13, juce::sendNotificationSync);
    CHECK(processor.getCurrentProgram() == 12);
    CHECK(processor.getActiveConsole() == Console::Ym2612);
    CHECK(box->getSelectedId() == 13);

    // The host changes the program, which the timer follows.
    processor.setCurrentProgram(28);
    flushTimer();
    CHECK(box->getSelectedId() == 29);
    setParam(processor, kConsoleId, 0.0f);
    flushTimer();
    CHECK(box->getSelectedId() == 0);

    // The same for an editor opened on another console.
    editor.reset();
    editor.reset(processor.createEditor());
    box = presetBoxOf(*editor);
    CHECK(box != nullptr && box->getSelectedId() == 0);
}

void typed_values_are_read_as_shown()
{
    RetroChipProcessor processor;

    auto* root = processor.apvts.getParameter(retrochip::ids::snesRootNote);
    const auto note = [root](const juce::String& typed) { return juce::roundToInt(root->convertFrom0to1(root->getValueForText(typed))); };
    CHECK(note("C4") == 60);
    CHECK(note("c4") == 60);
    CHECK(note(" C#4 ") == 61);
    CHECK(note("Bb3") == 58);
    CHECK(note("bb3") == 58);
    CHECK(note("b3") == 59);
    CHECK(note("A4") == 69);
    CHECK(note("C-1") == 0);
    CHECK(note("G9") == 127);
    CHECK(note("C-5") == 0);
    CHECK(note("B9") == 127);
    CHECK(note("60") == 60);
    CHECK(note(" 72 ") == 72);
    CHECK(note("200") == 127);
    CHECK(note("-3") == 0);
    CHECK(note("nonsense") == 60); // the default, not the bottom of the keyboard
    CHECK(note("") == 60);
    for (int i = 0; i <= 127; ++i)
        CHECK(note(root->getText(root->convertTo0to1(static_cast<float>(i)), 16)) == i);

    auto* delay = processor.apvts.getParameter(retrochip::ids::snesEchoDelay);
    const auto step = [delay](const juce::String& typed) { return juce::roundToInt(delay->convertFrom0to1(delay->getValueForText(typed))); };
    CHECK(step("64") == 4);
    CHECK(step("64 ms") == 4);
    CHECK(step("64ms") == 4);
    CHECK(step("70") == 4);
    CHECK(step("0") == 0);
    CHECK(step("15") == 1);
    CHECK(step("240") == 15);
    CHECK(step("1000") == 15);

    // On the other chip knobs a number is still a step.
    auto* attack = processor.apvts.getParameter(retrochip::ids::snesAttack);
    CHECK(juce::roundToInt(attack->convertFrom0to1(attack->getValueForText("5"))) == 5);

    // What the boxes on screen do with it.
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto* rootBox = dynamic_cast<juce::Slider*>(findById(*editor, retrochip::ids::snesRootNote));
    CHECK(rootBox != nullptr && juce::roundToInt(rootBox->getValueFromText("C4")) == 60);
    auto* delayKnob = findById(*editor, retrochip::ids::snesEchoDelay);
    CHECK(delayKnob != nullptr);
    if (delayKnob != nullptr)
        for (auto* child : delayKnob->getChildren())
            if (auto* slider = dynamic_cast<juce::Slider*>(child))
                CHECK(juce::roundToInt(slider->getValueFromText("64")) == 4);
}

void status_tooltip_has_a_window_to_appear_in()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    int windows = 0;
    for (auto* child : editor->getChildren())
        if (dynamic_cast<juce::TooltipWindow*>(child) != nullptr)
            ++windows;
    CHECK(windows == 1);
}

void vibrato_knobs_are_named_in_full()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    const auto check = [&](const char* id, const juce::String& shown, const juce::String& title)
    {
        auto* knob = findById(*editor, id);
        CHECK(knob != nullptr);
        if (knob == nullptr)
            return;
        for (auto* child : knob->getChildren())
        {
            if (auto* slider = dynamic_cast<juce::Slider*>(child))
                CHECK(slider->getTitle() == title);
            if (auto* label = dynamic_cast<juce::Label*>(child))
                CHECK(label->getText() == shown);
        }
    };
    check(retrochip::ids::vibratoRate, "Rate", "Vibrato Rate");
    check(retrochip::ids::vibratoDepth, "Depth", "Vibrato Depth");
}

void nes_state_loads_into_a_running_sdsp_instance()
{
    const auto state = loadReference("nes_state.bin");
    RetroChipProcessor processor;
    selectSdsp(processor);
    processor.prepareToPlay(kSampleRate, kBlock);
    run(processor, 2, 60);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(consoleValue(processor) == 0.0f);
}

// ---- SoundFont ----

using retrochip::LibraryEntry;
using retrochip::resolveInLibrary;
using retrochip::scanLibrary;
using retrochip::SoundFontBank;
using retrochip::SoundFontEngine;
using retrochip::SoundFontLoader;

constexpr int kSoundFontConsole = 3;
const juce::String kNoSoundFont = juce::String("No SoundFont: add .sf2 files to the folder");
const juce::String kNoInstrument = juce::String("Instrument not found: first instrument used");
// The same, when the project saved the instrument's name.
juce::String noInstrumentMessage(const juce::String& name)
{
    return "Instrument not found (" + name + "): first instrument used";
}

juce::String missingMessage(const juce::String& name) { return "File not found: " + name; }
juce::String unreadableMessage(const juce::String& name) { return "Unreadable file: " + name; }
juce::String tooLargeMessage(const juce::String& name) { return "File too large (1 GB limit): " + name; }

void writeBytes(const juce::File& file, const std::vector<std::uint8_t>& bytes)
{
    CHECK(file.getParentDirectory().createDirectory().wasOk());
    CHECK(file.replaceWithData(bytes.data(), bytes.size()));
}

void writeSf2(const juce::File& file, const testsf2::TestSf2Options& options = {})
{
    writeBytes(file, testsf2::buildTestSf2(options));
}

// A file with three instruments that differ: "Loud" at bank 0 program 0, "Soft" at bank 0 program 1 and "Far"
// (the same sound as Soft) at bank 8 program 5. Soft has 400 cB of attenuation, which the bank plays as
// 16 dB down (kAttenuationFix times the 4 dB TinySoundFont makes of it).
std::vector<std::uint8_t> threeInstruments()
{
    using namespace testsf2;
    const Zone loud { { kSampleModes, 1 }, { kSampleId, 1 } };
    const Zone soft { { kAttenuation, 400 }, { kSampleModes, 1 }, { kSampleId, 1 } };
    return buildSf2({ { "Loud", 0, 0, { { { kInstrument, 0 } } } },
                      { "Soft", 0, 1, { { { kInstrument, 1 } } } },
                      { "Far", 8, 5, { { { kInstrument, 1 } } } } },
                    { { "Loud", { loud } }, { "Soft", { soft } } });
}

// Delivers what the loader has finished, as the host's message loop would.
bool waitForLoad(RetroChipProcessor& processor)
{
    const auto giveUp = juce::Time::getMillisecondCounter() + 20000;
    while (processor.isSoundFontLoading())
    {
        if (juce::Time::getMillisecondCounter() > giveUp)
        {
            std::printf("  FAILED: a load never finished\n");
            ++failures;
            return false;
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    }
    return true;
}

void loadSoundFont(RetroChipProcessor& processor, const juce::File& root, const juce::String& path, int bank = -1, int program = -1)
{
    processor.setLibraryRoot(root);
    setConsole(processor, kSoundFontConsole);
    processor.prepareToPlay(kSampleRate, kBlock);
    processor.selectSoundFont(path, bank, program);
    waitForLoad(processor);
}

// The peak of one note, cut off afterwards so the next one starts clean.
double noteLevel(RetroChipProcessor& processor, int note = 69)
{
    const double level = run(processor, 8, note);
    allOff(processor);
    run(processor, 1);
    return level;
}

juce::MemoryBlock saveState(RetroChipProcessor& processor)
{
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    return state;
}

void loadState(RetroChipProcessor& processor, const juce::MemoryBlock& state)
{
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
}

std::unique_ptr<juce::XmlElement> stateXml(const juce::MemoryBlock& state)
{
    return juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
}

// A default state that names a file and an instrument, as a project saved earlier would.
juce::MemoryBlock stateNaming(const juce::String& path, int bank, int program, const juce::String& name = "Saved")
{
    RetroChipProcessor source;
    auto xml = stateXml(saveState(source));
    xml->setAttribute("sfPath", path);
    xml->setAttribute("sfBank", bank);
    xml->setAttribute("sfProgram", program);
    xml->setAttribute("sfName", name);
    juce::MemoryBlock state;
    juce::AudioProcessor::copyXmlToBinary(*xml, state);
    return state;
}

juce::StringArray pathsOf(const std::vector<LibraryEntry>& entries)
{
    juce::StringArray paths;
    for (const auto& entry : entries)
        paths.add(entry.relativePath);
    return paths;
}

void library_groups_and_sorts()
{
    TempFolder folder;
    const auto touch = [&](const char* path) { CHECK(folder.dir.getChildFile(path).create().wasOk()); };
    touch("Zeta/b.sf2");
    touch("Zeta/a.SF2");
    touch("Zeta/Track 10.sf2");
    touch("Zeta/Track 2.sf2");
    touch("Alpha/deep/deeper/x.sf2");
    touch("Alpha/notes.txt");
    touch("Alpha/y.sf3");
    touch("Alpha/z.sf2.zip");
    touch("Only text/readme.txt");
    touch("loose.sf2");
    touch("Other/mine.sf2");
    touch("kept.Sf2");

    juce::String message = "stale";
    const auto entries = scanLibrary(folder.dir, message);
    CHECK(message.isEmpty());
    CHECK(pathsOf(entries) == juce::StringArray({ "Alpha/deep/deeper/x.sf2", "Zeta/a.SF2", "Zeta/b.sf2", "Zeta/Track 2.sf2",
                                                  "Zeta/Track 10.sf2", "kept.Sf2", "loose.sf2", "Other/mine.sf2" }));
    if (entries.size() != 8)
        return;
    CHECK(entries[0].group == "Alpha"); // a deeper folder goes under its first-level parent
    CHECK(entries[0].name == "x");
    CHECK(entries[1].group == "Zeta");
    CHECK(entries[1].name == "a");
    for (size_t i = 5; i < entries.size(); ++i)
        CHECK(entries[i].group == "Other"); // last, with the root's files and a folder of that name together
    CHECK(entries[5].name == "kept");
}

void library_missing_folder_gives_a_message()
{
    TempFolder folder;
    juce::String message;
    CHECK(scanLibrary(folder.dir.getChildFile("not here"), message).empty());
    CHECK(message == kNoSoundFont);

    message.clear();
    CHECK(scanLibrary(folder.dir, message).empty()); // a folder with nothing to play says the same
    CHECK(message == kNoSoundFont);

    const auto file = folder.dir.getChildFile("plain file");
    CHECK(file.create().wasOk());
    message.clear();
    CHECK(scanLibrary(file, message).empty());
    CHECK(message == kNoSoundFont);
}

void unreadable_folder_gives_a_message()
{
    TempFolder folder;
    const auto root = folder.dir.getChildFile("Library");
    const auto good = root.getChildFile("Good/a.sf2");
    const auto locked = root.getChildFile("Locked");
    CHECK(good.create().wasOk());
    CHECK(locked.getChildFile("b.sf2").create().wasOk());

    juce::String message;
    if (kPosixFiles)
    {
        // A folder inside: what can be read is listed.
        CHECK(setMode(locked, 0));
        CHECK(pathsOf(scanLibrary(root, message)) == juce::StringArray({ "Good/a.sf2" }));
        CHECK(message == "Unreadable folder: Locked");
        CHECK(setMode(locked, 0755));

        // The library itself.
        CHECK(setMode(root, 0));
        message.clear();
        CHECK(scanLibrary(root, message).empty());
        CHECK(message == "Unreadable folder: Library");
        CHECK(setMode(root, 0755));
    }

    message.clear();
    CHECK(scanLibrary(root, message).size() == 2);
    CHECK(message.isEmpty());
}

void library_handles_accented_names()
{
    const auto utf8 = [](const char* text) { return juce::String::fromUTF8(text); };
    TempFolder folder;
    const auto accented = utf8("Éléphant/Thème à l'été.sf2");
    writeSf2(folder.dir.getChildFile(accented));
    CHECK(folder.dir.getChildFile("Delta/a.sf2").create().wasOk());
    CHECK(folder.dir.getChildFile("Zelda/b.sf2").create().wasOk());
    CHECK(folder.dir.getChildFile("Zelda/é.sf2").create().wasOk());
    CHECK(folder.dir.getChildFile("Zelda/f.sf2").create().wasOk());
    CHECK(folder.dir.getChildFile("Zelda/d.sf2").create().wasOk());
    // Decomposed, as an old disk stores names: an e and a combining accent.
    CHECK(folder.dir.getChildFile(utf8("Zelda/e\xCC\x80.sf2")).create().wasOk());

    juce::String message;
    const auto entries = scanLibrary(folder.dir, message);
    CHECK(message.isEmpty());
    CHECK(entries.size() == 7);
    if (entries.size() != 7)
        return;

    // In alphabetical order: an accented letter goes with its letter, not after the Z.
    CHECK(entries[0].group == "Delta");
    CHECK(entries[1].group == utf8("Éléphant"));
    CHECK(entries[2].group == "Zelda");
    CHECK(entries[2].name == "b");
    CHECK(entries[3].name == "d");
    CHECK(entries[6].name == "f");

    CHECK(entries[1].name == utf8("Thème à l'été"));
    CHECK(entries[1].relativePath == accented);
    CHECK(resolveInLibrary(folder.dir, entries[1].relativePath).existsAsFile());

    // It loads, and a saved state finds it again.
    RetroChipProcessor processor;
    loadSoundFont(processor, folder.dir, entries[1].relativePath);
    CHECK(processor.getSoundFontStatus().isEmpty());
    CHECK(noteLevel(processor) > 0.02);

    const auto state = saveState(processor);
    RetroChipProcessor restored;
    restored.setLibraryRoot(folder.dir);
    loadState(restored, state);
    waitForLoad(restored);
    CHECK(restored.getSoundFontPath() == accented);
    CHECK(restored.getSoundFontInstruments().size() == 1);
}

void resolve_rejects_paths_leaving_the_root()
{
    TempFolder folder;
    const auto root = folder.dir.getChildFile("Library");
    const auto inside = root.getChildFile("Group/a.sf2");
    CHECK(inside.create().wasOk());
    CHECK(folder.dir.getChildFile("outside.sf2").create().wasOk());

    for (const auto* path : { "", "..", "../outside.sf2", "Group/../../outside.sf2", "Group/..", "/etc/passwd", "//etc/passwd",
                              "~", "~/Music/a.sf2", "..\\outside.sf2", "Group\\..\\..\\outside.sf2", "\\etc\\passwd" })
    {
        if (resolveInLibrary(root, path) != juce::File())
        {
            std::printf("  FAILED: the path \"%s\" was accepted\n", path);
            ++failures;
        }
    }
    CHECK(resolveInLibrary(juce::File(), "a.sf2") == juce::File());

    CHECK(resolveInLibrary(root, "Group/a.sf2") == inside);
    CHECK(resolveInLibrary(root, "never/made.sf2") == root.getChildFile("never/made.sf2")); // the loader finds that out

    // Only a whole name is special, and these stay inside.
    CHECK(resolveInLibrary(root, "..hidden.sf2").getFullPathName() == root.getFullPathName() + "/..hidden.sf2");
    CHECK(resolveInLibrary(root, "a..b/c..sf2").getFullPathName() == root.getFullPathName() + "/a..b/c..sf2");
    CHECK(resolveInLibrary(root, "~tilde.sf2").getFullPathName() == root.getFullPathName() + "/~tilde.sf2");
}

void library_survives_a_symlink_loop()
{
    TempFolder folder;
    const auto root = folder.dir.getChildFile("Library");
    CHECK(root.getChildFile("a/x.sf2").create().wasOk());
    if (kPosixFiles)
        CHECK(root.getChildFile("a").createSymbolicLink(root.getChildFile("a/loop"), true));

    juce::String message;
    const auto entries = scanLibrary(root, message);
    CHECK(pathsOf(entries).contains("a/x.sf2"));
    for (const auto& entry : entries)
        CHECK(entry.relativePath.retainCharacters("/").length() <= 8);

    // Eight folders down is the limit, for real folders too.
    TempFolder deep;
    const auto eight = "1/2/3/4/5/6/7/8/in.sf2";
    const auto nine = "1/2/3/4/5/6/7/8/9/out.sf2";
    CHECK(deep.dir.getChildFile(eight).create().wasOk());
    CHECK(deep.dir.getChildFile(nine).create().wasOk());
    CHECK(pathsOf(scanLibrary(deep.dir, message)) == juce::StringArray({ eight }));
}

void library_default_is_in_the_music_folder()
{
    const auto expected = juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("RetroChip/SoundFonts");
    CHECK(retrochip::defaultLibraryRoot() == expected);

    RetroChipProcessor processor;
    CHECK(processor.getLibraryRoot() == expected);
    TempFolder folder;
    processor.setLibraryRoot(folder.dir);
    CHECK(processor.getLibraryRoot() == folder.dir);
}

void soundfont_parameters_match_the_spec()
{
    RetroChipProcessor processor;
    const auto check = [&](const char* id, float minimum, float maximum, float defaultValue)
    {
        auto* parameter = processor.apvts.getParameter(id);
        CHECK(parameter != nullptr);
        if (parameter == nullptr)
            return;
        const auto range = processor.apvts.getParameterRange(id);
        CHECK(std::abs(range.start - minimum) < 1e-3f);
        CHECK(std::abs(range.end - maximum) < 1e-3f);
        CHECK(std::abs(parameter->convertFrom0to1(parameter->getDefaultValue()) - defaultValue) < 1e-3f);
    };
    check(retrochip::ids::sfEchoMix, 0.0f, 100.0f, 0.0f);
    check(retrochip::ids::sfEchoDelay, 0.0f, 15.0f, 4.0f);
    check(retrochip::ids::sfEchoFeedback, 0.0f, 100.0f, 30.0f);

    CHECK(processor.apvts.getParameter(retrochip::ids::sfEchoDelay)->getText(processor.apvts.getParameter(retrochip::ids::sfEchoDelay)->convertTo0to1(15.0f), 32) == "240 ms");

    setConsole(processor, kSoundFontConsole);
    CHECK(processor.getActiveConsole() == Console::SoundFont);
    CHECK(static_cast<int>(Console::SoundFont) == 3);
    CHECK(dynamic_cast<SoundFontEngine*>(processor.engineFor(Console::SoundFont)) != nullptr);

    // The presets stay on the first three consoles.
    CHECK(processor.getNumPrograms() == 72);
    for (const auto& preset : retrochip::kPresets)
        CHECK(preset.console != kSoundFontConsole);
}

void soundfont_loads_and_plays()
{
    TempFolder folder;
    testsf2::TestSf2Options options;
    options.presets = { { "First", 0, 0 }, { "Second", 0, 3 } };
    writeSf2(folder.dir.getChildFile("Group/test.sf2"), options);

    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    setConsole(processor, kSoundFontConsole);
    processor.prepareToPlay(kSampleRate, kBlock);

    // No file yet: silence, and nothing to say.
    CHECK(run(processor, 4, 69) < 1e-6);
    allOff(processor);
    CHECK(processor.getSoundFontPath().isEmpty());
    CHECK(processor.getSoundFontInstruments().empty());
    CHECK(processor.getSoundFontStatus().isEmpty());
    CHECK(!processor.isSoundFontLoading());

    const auto revision = processor.getSoundFontRevision();
    processor.selectSoundFont("Group/test.sf2", -1, -1);
    CHECK(processor.isSoundFontLoading());
    CHECK(processor.getSoundFontPath() == "Group/test.sf2");
    waitForLoad(processor);
    CHECK(processor.getSoundFontRevision() != revision);

    CHECK(processor.getSoundFontPath() == "Group/test.sf2");
    CHECK(processor.getSoundFontStatus().isEmpty());
    CHECK(processor.getSoundFontInstrumentIndex() == 0);
    const auto instruments = processor.getSoundFontInstruments();
    CHECK(instruments.size() == 2);
    if (instruments.size() == 2)
    {
        CHECK(instruments[0].name == "First");
        CHECK(instruments[0].bank == 0);
        CHECK(instruments[0].program == 0);
        CHECK(instruments[1].name == "Second");
        CHECK(instruments[1].program == 3);
    }

    // The next block adopts it, and the zones and the pitch are the font's.
    auto* engine = dynamic_cast<SoundFontEngine*>(processor.engineFor(Console::SoundFont));
    CHECK(engine != nullptr);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(120)), 0);
    const auto samples = processWith(processor, 40, on);
    CHECK(peakOf(samples, 4096) > 0.02);
    CHECK(std::abs(estimateFrequency(samples, kSampleRate, 4096) - 440.0) < 2.0);
    if (engine != nullptr)
        CHECK(engine->activeVoices() == 1);
}

void instrument_choice_changes_the_sound()
{
    TempFolder folder;
    writeBytes(folder.dir.getChildFile("three.sf2"), threeInstruments());

    RetroChipProcessor processor;
    loadSoundFont(processor, folder.dir, "three.sf2", 0, 1);
    CHECK(processor.getSoundFontInstrumentIndex() == 1);
    CHECK(processor.getSoundFontStatus().isEmpty());
    const double soft = noteLevel(processor);

    const auto revision = processor.getSoundFontRevision();
    processor.selectSoundFontInstrument(0);
    CHECK(processor.getSoundFontInstrumentIndex() == 0);
    CHECK(processor.getSoundFontRevision() != revision);
    const double loud = noteLevel(processor);
    CHECK(loud > 0.02);
    CHECK(loud > soft * 4.0);

    // A choice outside the list changes nothing.
    processor.selectSoundFontInstrument(99);
    CHECK(processor.getSoundFontInstrumentIndex() == 0);
    processor.selectSoundFontInstrument(-1);
    CHECK(processor.getSoundFontInstrumentIndex() == 0);
    CHECK(std::abs(noteLevel(processor) - loud) < loud * 0.01);

    // A new instrument cuts the note that sounds.
    auto* engine = dynamic_cast<SoundFontEngine*>(processor.engineFor(Console::SoundFont));
    CHECK(engine != nullptr);
    run(processor, 2, 69);
    if (engine != nullptr)
        CHECK(engine->activeVoices() == 1);
    processor.selectSoundFontInstrument(2);
    run(processor, 1);
    if (engine != nullptr)
        CHECK(engine->activeVoices() == 0);
}

void a_new_file_starts_on_its_first_instrument()
{
    TempFolder folder;
    testsf2::TestSf2Options options;
    options.presets = { { "Five", 2, 5 }, { "Seven", 2, 7 } };
    writeSf2(folder.dir.getChildFile("no zero.sf2"), options);

    // A file picked from the menu has no saved instrument: its first is no cause for a message.
    RetroChipProcessor processor;
    loadSoundFont(processor, folder.dir, "no zero.sf2");
    CHECK(processor.getSoundFontStatus().isEmpty());
    CHECK(processor.getSoundFontInstrumentIndex() == 0);
    CHECK(noteLevel(processor) > 0.02);

    const auto xml = stateXml(saveState(processor));
    CHECK(xml->getIntAttribute("sfBank") == 2);
    CHECK(xml->getIntAttribute("sfProgram") == 5);
    CHECK(xml->getStringAttribute("sfName") == "Five");
}

void instrument_names_decode_as_utf8_or_latin1()
{
    CHECK(retrochip::decodeInstrumentName("Plain") == "Plain");
    CHECK(retrochip::decodeInstrumentName("Caf\xC3\xA9") == juce::String::fromUTF8("Café"));
    CHECK(retrochip::decodeInstrumentName("Caf\xE9") == juce::String::fromUTF8("Café"));
    CHECK(retrochip::decodeInstrumentName("\xC9t\xE9 \xE0 Noel") == juce::String::fromUTF8("Été à Noel"));
    CHECK(retrochip::decodeInstrumentName("").isEmpty());

    TempFolder folder;
    testsf2::TestSf2Options options;
    options.presets = { { "Caf\xE9", 0, 0 }, { "Caf\xC3\xA9", 0, 1 } };
    writeSf2(folder.dir.getChildFile("names.sf2"), options);
    RetroChipProcessor processor;
    loadSoundFont(processor, folder.dir, "names.sf2");
    const auto instruments = processor.getSoundFontInstruments();
    CHECK(instruments.size() == 2);
    if (instruments.size() == 2)
    {
        CHECK(instruments[0].name == juce::String::fromUTF8("Café"));
        CHECK(instruments[1].name == juce::String::fromUTF8("Café"));
    }
}

void load_refuses_unreadable_and_too_large()
{
    TempFolder folder;
    writeBytes(folder.dir.getChildFile("good.sf2"), threeInstruments());
    const auto garbage = folder.dir.getChildFile("garbage.sf2");
    CHECK(garbage.replaceWithText("this is not a SoundFont"));
    const auto empty = folder.dir.getChildFile("empty.sf2");
    CHECK(empty.create().wasOk());
    const auto locked = folder.dir.getChildFile("locked.sf2");
    const auto huge = folder.dir.getChildFile("huge.sf2");
    const auto hugeLocked = folder.dir.getChildFile("huge locked.sf2");
    CHECK(SoundFontLoader::kMaxFileBytes == 1024LL * 1024 * 1024);
    std::vector<std::pair<const char*, juce::String>> refused {
        { "garbage.sf2", unreadableMessage("garbage.sf2") }, { "empty.sf2", unreadableMessage("empty.sf2") },
        { "gone/absent.sf2", missingMessage("absent.sf2") }, { "../good.sf2", missingMessage("good.sf2") },
    };
    if (kPosixFiles)
    {
        writeSf2(locked);
        CHECK(setMode(locked, 0));

        // Holes only, one byte over the limit, and it takes no room on the disk. The second cannot be opened, so it
        // can only be called too large if the size is checked before any read is tried.
        for (const auto& file : { huge, hugeLocked })
        {
            CHECK(file.create().wasOk());
            CHECK(setSparseSize(file, SoundFontLoader::kMaxFileBytes + 1));
            CHECK(file.getSize() == SoundFontLoader::kMaxFileBytes + 1);
        }
        CHECK(setMode(hugeLocked, 0));
        refused.push_back({ "locked.sf2", unreadableMessage("locked.sf2") });
        refused.push_back({ "huge.sf2", tooLargeMessage("huge.sf2") });
        refused.push_back({ "huge locked.sf2", tooLargeMessage("huge locked.sf2") });
    }

    const auto tryEach = [&](RetroChipProcessor& processor)
    {
        const auto path = processor.getSoundFontPath();
        const auto index = processor.getSoundFontInstrumentIndex();
        const auto count = processor.getSoundFontInstruments().size();
        const auto level = noteLevel(processor);
        for (const auto& [file, message] : refused)
        {
            processor.selectSoundFont(file, 0, 0);
            waitForLoad(processor);
            CHECK(processor.getSoundFontStatus() == message);

            // The current sound stays, and so does what a saved project would reopen.
            CHECK(processor.getSoundFontPath() == path);
            CHECK(processor.getSoundFontInstrumentIndex() == index);
            CHECK(processor.getSoundFontInstruments().size() == count);
            CHECK(processor.getRetiredBankCount() == 0);
            CHECK(std::abs(noteLevel(processor) - level) <= level * 0.01);
            const auto xml = stateXml(saveState(processor));
            CHECK(xml->getStringAttribute("sfPath") == path);
        }
    };

    RetroChipProcessor playing;
    loadSoundFont(playing, folder.dir, "good.sf2");
    CHECK(noteLevel(playing) > 0.02);
    tryEach(playing);

    // A later good file clears the message.
    playing.selectSoundFont("good.sf2", 0, 1);
    waitForLoad(playing);
    CHECK(playing.getSoundFontStatus().isEmpty());
    CHECK(playing.getSoundFontInstrumentIndex() == 1);

    // With nothing loaded there is nothing to keep, and no crash.
    RetroChipProcessor idle;
    idle.setLibraryRoot(folder.dir);
    setConsole(idle, kSoundFontConsole);
    idle.prepareToPlay(kSampleRate, kBlock);
    tryEach(idle);
    CHECK(noteLevel(idle) < 1e-6);

    if (kPosixFiles)
    {
        CHECK(setMode(locked, 0644));
        CHECK(setMode(hugeLocked, 0644));
    }
}

void last_request_wins()
{
    TempFolder folder;
    testsf2::TestSf2Options quiet;
    quiet.highAmplitude = 0.1;
    writeSf2(folder.dir.getChildFile("quiet.sf2"), quiet);
    testsf2::TestSf2Options loud;
    loud.highAmplitude = 0.8;
    writeSf2(folder.dir.getChildFile("loud.sf2"), loud);

    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    setConsole(processor, kSoundFontConsole);
    processor.prepareToPlay(kSampleRate, kBlock);

    for (int i = 0; i < 10; ++i)
        processor.selectSoundFont("quiet.sf2", -1, -1);
    processor.selectSoundFont("loud.sf2", -1, -1);
    waitForLoad(processor);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20); // anything still on its way
    CHECK(processor.getSoundFontPath() == "loud.sf2");
    const double level = noteLevel(processor);
    CHECK(level > 0.05);

    // A refused file also counts as the last one, and the one before it does not arrive later.
    processor.selectSoundFont("quiet.sf2", -1, -1);
    processor.selectSoundFont("absent.sf2", -1, -1);
    waitForLoad(processor);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    CHECK(processor.getSoundFontPath() == "loud.sf2");
    CHECK(processor.getSoundFontStatus() == missingMessage("absent.sf2"));
    CHECK(std::abs(noteLevel(processor) - level) <= level * 0.01);

    // Finished on the loader's thread and on its way when the next request came: no message loop runs in between,
    // so it is waiting for one. It must not take effect when it gets there.
    const auto finishUndelivered = [&]
    {
        processor.selectSoundFont("quiet.sf2", -1, -1);
        juce::Thread::sleep(200);
    };
    finishUndelivered();
    processor.selectSoundFont("absent.sf2", -1, -1);
    waitForLoad(processor);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    CHECK(processor.getSoundFontPath() == "loud.sf2");
    CHECK(processor.getSoundFontStatus() == missingMessage("absent.sf2"));
    CHECK(std::abs(noteLevel(processor) - level) <= level * 0.01);

    finishUndelivered();
    processor.selectSoundFont({});
    CHECK(!processor.isSoundFontLoading());
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    CHECK(processor.getSoundFontPath().isEmpty());
    CHECK(processor.getSoundFontInstruments().empty());
    CHECK(noteLevel(processor) < 1e-6);
}

void bank_built_before_prepare_plays_at_the_right_rate()
{
    TempFolder folder;
    writeSf2(folder.dir.getChildFile("test.sf2"));

    const auto check = [](RetroChipProcessor& processor, double rate)
    {
        juce::MidiBuffer on;
        on.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(120)), 0);
        const auto samples = processWith(processor, 40, on);
        CHECK(std::abs(estimateFrequency(samples, rate, 8192) - 440.0) < 2.0);
        allOff(processor);
    };

    // Asked for before the host says the rate: the block after prepareToPlay adopts it at the right one.
    RetroChipProcessor early;
    early.setLibraryRoot(folder.dir);
    setConsole(early, kSoundFontConsole);
    early.selectSoundFont("test.sf2", -1, -1);
    waitForLoad(early);
    early.prepareToPlay(96000.0, kBlock);
    check(early, 96000.0);

    // The bank in use follows a new rate, and is not loaded again.
    early.prepareToPlay(44100.0, kBlock);
    CHECK(!early.isSoundFontLoading());
    check(early, 44100.0);

    // A load that was running across prepareToPlay.
    RetroChipProcessor during;
    during.setLibraryRoot(folder.dir);
    setConsole(during, kSoundFontConsole);
    during.prepareToPlay(44100.0, kBlock);
    during.selectSoundFont("test.sf2", -1, -1);
    during.prepareToPlay(88200.0, kBlock);
    waitForLoad(during);
    check(during, 88200.0);

    // A request after prepareToPlay builds the bank at that rate, so adopting it has nothing to change.
    during.selectSoundFont("test.sf2", -1, -1);
    waitForLoad(during);
    check(during, 88200.0);
}

void adopting_a_bank_allocates_nothing()
{
    TempFolder folder;
    writeSf2(folder.dir.getChildFile("test.sf2"));

    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    setConsole(processor, kSoundFontConsole);
    processor.selectSoundFont("test.sf2", -1, -1); // at the default rate, and another rate comes next
    waitForLoad(processor);
    processor.prepareToPlay(96000.0, kBlock);

    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 0);
    const auto before = retrochip::soundfont::allocationCount();
    processor.processBlock(buffer, midi);
    CHECK(retrochip::soundfont::allocationCount() == before);
    CHECK(peakOf(std::vector<float>(buffer.getReadPointer(0), buffer.getReadPointer(0) + kBlock)) > 0.001);
}

// The handover, step by step: the old bank stays until the audio thread has taken the new one.
void replaced_bank_waits_for_the_audio_thread()
{
    TempFolder folder;
    writeSf2(folder.dir.getChildFile("a.sf2"));
    writeSf2(folder.dir.getChildFile("b.sf2"));
    const auto select = [&](RetroChipProcessor& p, const char* path)
    {
        p.selectSoundFont(path, -1, -1);
        waitForLoad(p);
    };

    // No audio running: nothing piles up, since the audio thread has seen none of them.
    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    setConsole(processor, kSoundFontConsole);
    for (int i = 0; i < 6; ++i)
    {
        select(processor, i % 2 == 0 ? "a.sf2" : "b.sf2");
        CHECK(processor.getRetiredBankCount() == 0);
    }

    processor.prepareToPlay(kSampleRate, kBlock);
    auto* engine = dynamic_cast<SoundFontEngine*>(processor.engineFor(Console::SoundFont));
    CHECK(engine != nullptr);
    if (engine == nullptr)
        return;
    CHECK(run(processor, 4, 69) > 0.02);
    CHECK(engine->activeVoices() == 1);

    select(processor, "a.sf2");
    CHECK(processor.getRetiredBankCount() == 1);
    processor.releaseRetiredBanks();
    CHECK(processor.getRetiredBankCount() == 1);
    CHECK(engine->activeVoices() == 1); // the engine has not been told yet
    select(processor, "b.sf2");         // not taken yet: the one in between is dropped
    CHECK(processor.getRetiredBankCount() == 1);

    run(processor, 1);
    CHECK(engine->activeVoices() == 0);
    processor.releaseRetiredBanks();
    CHECK(processor.getRetiredBankCount() == 0);

    // With no editor open, the processor frees it by itself.
    CHECK(run(processor, 2, 69) > 0.02);
    select(processor, "a.sf2");
    CHECK(processor.getRetiredBankCount() == 1);
    run(processor, 1);
    const auto giveUp = juce::Time::getMillisecondCounter() + 5000;
    while (processor.getRetiredBankCount() != 0 && juce::Time::getMillisecondCounter() < giveUp)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    CHECK(processor.getRetiredBankCount() == 0);
}

void state_round_trips_file_and_instrument()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("Group/three.sf2");
    writeBytes(file, threeInstruments());

    RetroChipProcessor processor;
    loadSoundFont(processor, folder.dir, "Group/three.sf2");
    processor.selectSoundFontInstrument(2);
    setParam(processor, retrochip::ids::sfEchoMix, 40.0f);
    const auto state = saveState(processor);

    const auto xml = stateXml(state);
    CHECK(xml != nullptr);
    if (xml == nullptr)
        return;
    CHECK(xml->getStringAttribute("sfPath") == "Group/three.sf2");
    CHECK(xml->getIntAttribute("sfBank") == 8);
    CHECK(xml->getIntAttribute("sfProgram") == 5);
    CHECK(xml->getStringAttribute("sfName") == "Far");

    RetroChipProcessor restored;
    restored.setLibraryRoot(folder.dir);
    loadState(restored, state);
    waitForLoad(restored);
    CHECK(restored.getActiveConsole() == Console::SoundFont);
    CHECK(restored.getSoundFontPath() == "Group/three.sf2");
    CHECK(restored.getSoundFontInstrumentIndex() == 2);
    CHECK(restored.getSoundFontStatus().isEmpty());
    CHECK(std::abs(getParam(restored, retrochip::ids::sfEchoMix) - 40.0f) < 0.5f);
    CHECK(!restored.apvts.state.hasProperty("sfPath")); // the state's own copy is not left among the parameters

    // Saved again, with nothing changed, it says the same.
    const auto again = stateXml(saveState(restored));
    CHECK(again->getStringAttribute("sfPath") == "Group/three.sf2");
    CHECK(again->getIntAttribute("sfBank") == 8);
    CHECK(again->getIntAttribute("sfProgram") == 5);
    CHECK(again->getStringAttribute("sfName") == "Far");

    // The bank and the program outlive a file that gained instruments before this one.
    using namespace testsf2;
    const Zone loud { { kSampleModes, 1 }, { kSampleId, 1 } };
    const Zone soft { { kAttenuation, 400 }, { kSampleModes, 1 }, { kSampleId, 1 } };
    writeBytes(file, buildSf2({ { "New", 0, 7, { { { kInstrument, 0 } } } },
                                { "Other", 1, 0, { { { kInstrument, 0 } } } },
                                { "Loud", 0, 0, { { { kInstrument, 0 } } } },
                                { "Soft", 0, 1, { { { kInstrument, 1 } } } },
                                { "Far", 8, 5, { { { kInstrument, 1 } } } } },
                              { { "Loud", { loud } }, { "Soft", { soft } } }));
    RetroChipProcessor grown;
    grown.setLibraryRoot(folder.dir);
    grown.prepareToPlay(kSampleRate, kBlock);
    loadState(grown, state);
    waitForLoad(grown);
    const auto instruments = grown.getSoundFontInstruments();
    CHECK(instruments.size() == 5);
    const auto index = grown.getSoundFontInstrumentIndex();
    CHECK(index == 4);
    if (index >= 0 && index < static_cast<int>(instruments.size()))
        CHECK(instruments[static_cast<size_t>(index)].name == "Far");
    CHECK(grown.getSoundFontStatus().isEmpty());
}

void state_with_missing_file_is_silent_with_message()
{
    TempFolder folder;
    writeSf2(folder.dir.getChildFile("playing.sf2"));

    // The rest of the state loads, even in an instance that was playing something else.
    juce::MemoryBlock gainState;
    {
        RetroChipProcessor source;
        setParam(source, retrochip::ids::gain, -12.0f);
        setConsole(source, kSoundFontConsole);
        auto sourceXml = stateXml(saveState(source));
        sourceXml->setAttribute("sfPath", "Group/gone.sf2");
        sourceXml->setAttribute("sfBank", 8);
        sourceXml->setAttribute("sfProgram", 5);
        sourceXml->setAttribute("sfName", "Saved");
        juce::AudioProcessor::copyXmlToBinary(*sourceXml, gainState);
    }

    RetroChipProcessor processor;
    loadSoundFont(processor, folder.dir, "playing.sf2");
    CHECK(noteLevel(processor) > 0.02);
    loadState(processor, gainState);
    waitForLoad(processor);
    CHECK(processor.getSoundFontStatus() == missingMessage("gone.sf2"));
    CHECK(processor.getSoundFontInstruments().empty());
    CHECK(std::abs(getParam(processor, retrochip::ids::gain) + 12.0f) < 0.01f);
    CHECK(noteLevel(processor) < 1e-6);
    run(processor, 1);
    processor.releaseRetiredBanks();
    CHECK(processor.getRetiredBankCount() == 0);

    // Saving it again keeps what the project asked for, so that opening that save with the file back in place
    // plays it (below); a file that comes back while this instance stays open is the refresh button's job.
    const auto resaved = saveState(processor);
    const auto resavedXml = stateXml(resaved);
    CHECK(resavedXml->getStringAttribute("sfPath") == "Group/gone.sf2");
    CHECK(resavedXml->getIntAttribute("sfBank") == 8);
    CHECK(resavedXml->getIntAttribute("sfProgram") == 5);
    CHECK(resavedXml->getStringAttribute("sfName") == "Saved");
    CHECK(processor.getSoundFontPath() == "Group/gone.sf2");

    // A new instrument pick has nothing to apply to.
    processor.selectSoundFontInstrument(0);
    CHECK(stateXml(saveState(processor))->getIntAttribute("sfBank") == 8);

    writeBytes(folder.dir.getChildFile("Group/gone.sf2"), threeInstruments());
    loadState(processor, resaved);
    waitForLoad(processor);
    CHECK(processor.getSoundFontStatus().isEmpty());
    CHECK(processor.getSoundFontInstruments().size() == 3);
    CHECK(processor.getSoundFontInstrumentIndex() == 2);
    CHECK(noteLevel(processor) > 1e-5);
}

void state_with_unreadable_file_keeps_the_sound_or_the_reference()
{
    TempFolder folder;
    writeSf2(folder.dir.getChildFile("playing.sf2"));
    CHECK(folder.dir.getChildFile("bad.sf2").replaceWithText("not a SoundFont"));
    const auto state = stateNaming("bad.sf2", 0, 3);

    // Something is playing: it stays, with a message, and a save describes what plays.
    RetroChipProcessor playing;
    loadSoundFont(playing, folder.dir, "playing.sf2");
    const auto level = noteLevel(playing);
    CHECK(level > 0.02);
    const auto bank = stateXml(saveState(playing))->getIntAttribute("sfBank");
    loadState(playing, state);
    waitForLoad(playing);
    CHECK(playing.getSoundFontStatus() == unreadableMessage("bad.sf2"));
    CHECK(playing.getSoundFontPath() == "playing.sf2");
    CHECK(stateXml(saveState(playing))->getStringAttribute("sfPath") == "playing.sf2");
    CHECK(stateXml(saveState(playing))->getIntAttribute("sfBank") == bank);
    setConsole(playing, kSoundFontConsole);
    run(playing, 1);
    CHECK(std::abs(noteLevel(playing) - level) <= level * 0.01);

    // Nothing is playing: the project keeps its file, in case it can be read another day.
    RetroChipProcessor fresh;
    fresh.setLibraryRoot(folder.dir);
    fresh.prepareToPlay(kSampleRate, kBlock);
    loadState(fresh, state);
    waitForLoad(fresh);
    CHECK(fresh.getSoundFontStatus() == unreadableMessage("bad.sf2"));
    const auto xml = stateXml(saveState(fresh));
    CHECK(xml->getStringAttribute("sfPath") == "bad.sf2");
    CHECK(xml->getIntAttribute("sfProgram") == 3);
}

void state_with_missing_instrument_uses_the_first()
{
    TempFolder folder;
    writeBytes(folder.dir.getChildFile("three.sf2"), threeInstruments());
    const auto state = stateNaming("three.sf2", 3, 33, "Lost");

    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    setConsole(processor, kSoundFontConsole);
    processor.prepareToPlay(kSampleRate, kBlock);
    loadState(processor, state);
    waitForLoad(processor);
    setConsole(processor, kSoundFontConsole);
    CHECK(processor.getSoundFontStatus() == noInstrumentMessage("Lost")); // the project's own name for it
    CHECK(processor.getSoundFontInstrumentIndex() == 0);
    CHECK(processor.getSoundFontInstruments().size() == 3);
    CHECK(noteLevel(processor) > 0.02); // the first is the loud one

    // Until a choice is made, the project still names the instrument it was saved with.
    auto xml = stateXml(saveState(processor));
    CHECK(xml->getIntAttribute("sfBank") == 3);
    CHECK(xml->getIntAttribute("sfProgram") == 33);
    CHECK(xml->getStringAttribute("sfName") == "Lost");

    // A choice ends the message and replaces it.
    processor.selectSoundFontInstrument(1);
    CHECK(processor.getSoundFontStatus().isEmpty());
    xml = stateXml(saveState(processor));
    CHECK(xml->getIntAttribute("sfBank") == 0);
    CHECK(xml->getIntAttribute("sfProgram") == 1);
    CHECK(xml->getStringAttribute("sfName") == "Soft");

    // Asked for here, an instrument has no saved name, and the message leaves the brackets out.
    processor.selectSoundFont("three.sf2", 3, 33);
    waitForLoad(processor);
    CHECK(processor.getSoundFontStatus() == kNoInstrument);

    // A project that saved no name is the same.
    juce::MemoryBlock nameless;
    {
        auto bare = stateXml(stateNaming("three.sf2", 3, 33));
        bare->removeAttribute("sfName");
        juce::AudioProcessor::copyXmlToBinary(*bare, nameless);
    }
    loadState(processor, nameless);
    waitForLoad(processor);
    CHECK(processor.getSoundFontStatus() == kNoInstrument);
}

// The file a project names comes back while the instance is open: nothing was loading, nothing played, and the
// refresh button asks the processor to try the saved choice again.
void retry_loads_a_file_that_came_back()
{
    TempFolder folder;
    const auto path = juce::String("Group/gone.sf2");

    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    processor.prepareToPlay(kSampleRate, kBlock);
    processor.retrySoundFont(); // no file held: nothing to do
    CHECK(!processor.isSoundFontLoading());

    loadState(processor, stateNaming(path, 8, 5, "Far"));
    waitForLoad(processor);
    setConsole(processor, kSoundFontConsole);
    CHECK(processor.getSoundFontStatus() == missingMessage("gone.sf2"));
    CHECK(noteLevel(processor) < 1e-6);

    // Still not there: the same message, and the project keeps its choice.
    processor.retrySoundFont();
    waitForLoad(processor);
    CHECK(processor.getSoundFontStatus() == missingMessage("gone.sf2"));
    CHECK(stateXml(saveState(processor))->getIntAttribute("sfBank") == 8);

    writeBytes(folder.dir.getChildFile(path), threeInstruments());
    processor.retrySoundFont();
    CHECK(processor.isSoundFontLoading());
    waitForLoad(processor);
    CHECK(processor.getSoundFontStatus().isEmpty());
    CHECK(processor.getSoundFontInstruments().size() == 3);
    CHECK(processor.getSoundFontInstrumentIndex() == 2); // the saved bank and program, not the first
    CHECK(noteLevel(processor) > 0.005);
    const auto xml = stateXml(saveState(processor));
    CHECK(xml->getStringAttribute("sfPath") == path);
    CHECK(xml->getIntAttribute("sfBank") == 8);
    CHECK(xml->getIntAttribute("sfProgram") == 5);
    CHECK(xml->getStringAttribute("sfName") == "Far");

    // With a bank playing, there is nothing to retry.
    processor.retrySoundFont();
    CHECK(!processor.isSoundFontLoading());
    CHECK(processor.getSoundFontInstrumentIndex() == 2);

    // A file that could not be read, and nothing playing, is the same.
    CHECK(folder.dir.getChildFile("bad.sf2").replaceWithText("not a SoundFont"));
    RetroChipProcessor fresh;
    fresh.setLibraryRoot(folder.dir);
    fresh.prepareToPlay(kSampleRate, kBlock);
    loadState(fresh, stateNaming("bad.sf2", 8, 5, "Far"));
    waitForLoad(fresh);
    setConsole(fresh, kSoundFontConsole);
    CHECK(fresh.getSoundFontStatus() == unreadableMessage("bad.sf2"));
    CHECK(noteLevel(fresh) < 1e-6);
    writeBytes(folder.dir.getChildFile("bad.sf2"), threeInstruments());
    fresh.retrySoundFont();
    waitForLoad(fresh);
    CHECK(fresh.getSoundFontStatus().isEmpty());
    CHECK(fresh.getSoundFontInstrumentIndex() == 2);
    CHECK(noteLevel(fresh) > 0.005);
}

void state_with_escaping_path_is_treated_as_missing()
{
    TempFolder folder;
    const auto root = folder.dir.getChildFile("Library");
    CHECK(root.createDirectory().wasOk());
    const auto outside = folder.dir.getChildFile("outside.sf2");
    writeSf2(outside); // a real, playable file: only the path keeps it from loading

    for (const auto& path : { juce::String("../outside.sf2"), juce::String("sub/../../outside.sf2"), outside.getFullPathName() })
    {
        RetroChipProcessor processor;
        processor.setLibraryRoot(root);
        setConsole(processor, kSoundFontConsole);
        processor.prepareToPlay(kSampleRate, kBlock);
        loadState(processor, stateNaming(path, 0, 0));
        CHECK(!processor.isSoundFontLoading()); // it never reaches the loader
        CHECK(processor.getSoundFontStatus() == missingMessage("outside.sf2"));
        CHECK(processor.getSoundFontInstruments().empty());
        setConsole(processor, kSoundFontConsole);
        CHECK(noteLevel(processor) < 1e-6);
    }
}

void state_keeps_a_load_in_progress()
{
    TempFolder folder;
    writeBytes(folder.dir.getChildFile("three.sf2"), threeInstruments());
    writeSf2(folder.dir.getChildFile("other.sf2"));
    const auto state = stateNaming("three.sf2", 8, 5, "Far");

    // No message loop has run, so the loader's result cannot have arrived, whatever it has done by now. A host
    // that saves at this moment must not lose the file.
    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    processor.prepareToPlay(kSampleRate, kBlock);
    loadState(processor, state);
    CHECK(processor.isSoundFontLoading());
    CHECK(processor.getSoundFontPath() == "three.sf2");
    auto xml = stateXml(saveState(processor));
    CHECK(xml->getStringAttribute("sfPath") == "three.sf2");
    CHECK(xml->getIntAttribute("sfBank") == 8);
    CHECK(xml->getIntAttribute("sfProgram") == 5);
    CHECK(xml->getStringAttribute("sfName") == "Far");

    waitForLoad(processor);
    CHECK(processor.getSoundFontInstrumentIndex() == 2);

    // The same when a loaded file is being replaced.
    processor.selectSoundFont("other.sf2", -1, -1);
    xml = stateXml(saveState(processor));
    CHECK(xml->getStringAttribute("sfPath") == "other.sf2");
    waitForLoad(processor);

    // A state with no file ends a load that is under way.
    RetroChipProcessor plain;
    const auto noFile = saveState(plain);
    processor.selectSoundFont("three.sf2", 0, 0);
    loadState(processor, noFile);
    CHECK(!processor.isSoundFontLoading());
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    CHECK(processor.getSoundFontPath().isEmpty());
    CHECK(processor.getSoundFontInstruments().empty());
}

void old_state_opens_without_soundfont()
{
    const auto old = loadReference("nes_state.bin");

    RetroChipProcessor fresh;
    loadState(fresh, old);
    CHECK(!fresh.isSoundFontLoading());
    CHECK(fresh.getSoundFontPath().isEmpty());
    CHECK(fresh.getSoundFontStatus().isEmpty()); // no file asked for, so nothing to complain of
    CHECK(fresh.getSoundFontInstruments().empty());
    CHECK(consoleValue(fresh) == 0.0f);
    CHECK(fresh.getCurrentProgram() == 4);
    CHECK(!stateXml(saveState(fresh))->hasAttribute("sfPath"));

    // In an instance with a file loaded, the file goes.
    TempFolder folder;
    writeSf2(folder.dir.getChildFile("test.sf2"));
    RetroChipProcessor running;
    loadSoundFont(running, folder.dir, "test.sf2");
    CHECK(noteLevel(running) > 0.02);
    loadState(running, old);
    CHECK(!running.isSoundFontLoading());
    CHECK(running.getSoundFontPath().isEmpty());
    CHECK(running.getSoundFontStatus().isEmpty());
    CHECK(running.getSoundFontInstruments().empty());
    CHECK(!stateXml(saveState(running))->hasAttribute("sfPath"));
    setConsole(running, kSoundFontConsole);
    CHECK(noteLevel(running) < 1e-6);
}

void deleted_file_keeps_playing_until_reopen()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("Group/test.sf2");
    writeSf2(file);

    RetroChipProcessor processor;
    loadSoundFont(processor, folder.dir, "Group/test.sf2");
    const auto level = noteLevel(processor);
    CHECK(level > 0.02);

    CHECK(file.deleteFile());
    CHECK(std::abs(noteLevel(processor) - level) <= level * 0.01);
    CHECK(processor.getSoundFontStatus().isEmpty());
    const auto state = saveState(processor);
    CHECK(stateXml(state)->getStringAttribute("sfPath") == "Group/test.sf2");

    RetroChipProcessor reopened;
    reopened.setLibraryRoot(folder.dir);
    reopened.prepareToPlay(kSampleRate, kBlock);
    loadState(reopened, state);
    waitForLoad(reopened);
    CHECK(reopened.getSoundFontStatus() == missingMessage("test.sf2"));
    CHECK(reopened.getSoundFontInstruments().empty());
    CHECK(noteLevel(reopened) < 1e-6);
}

void console_change_stops_soundfont_notes()
{
    TempFolder folder;
    writeSf2(folder.dir.getChildFile("test.sf2"));
    RetroChipProcessor processor;
    loadSoundFont(processor, folder.dir, "test.sf2");
    auto* engine = dynamic_cast<SoundFontEngine*>(processor.engineFor(Console::SoundFont));
    CHECK(engine != nullptr);
    if (engine == nullptr)
        return;

    CHECK(run(processor, 4, 69) > 0.02);
    CHECK(engine->activeVoices() == 1);

    // Held by the pedal too, and left: nothing stays stuck.
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer pedal;
    pedal.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    processor.processBlock(buffer, pedal);

    setConsole(processor, 0);
    run(processor, 1);
    CHECK(engine->activeVoices() == 0);

    // Back on it, nothing is left over, and it plays as usual.
    setConsole(processor, kSoundFontConsole);
    CHECK(run(processor, 2) < 1e-6);
    juce::MidiBuffer pedalUp;
    pedalUp.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
    processor.processBlock(buffer, pedalUp);
    CHECK(run(processor, 4, 69) > 0.02);
    CHECK(engine->activeVoices() == 1);

    // The other way round: the 2A03's note stops on arrival here.
    setConsole(processor, 0);
    run(processor, 1);
    CHECK(run(processor, 2, 69) > 0.02);
    setConsole(processor, kSoundFontConsole);
    CHECK(run(processor, 1) < 1e-6);
    CHECK(engine->activeVoices() == 0);
}

void reported_latency_is_zero_on_soundfont()
{
    const auto flushMessages = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil(20); };
    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(processor.getLatencySamples() > 0); // the 2A03 has some, so going to zero is a change

    setConsole(processor, kSoundFontConsole);
    flushMessages();
    CHECK(processor.getLatencySamples() == 0);

    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(processor.getLatencySamples() == 0);

    setConsole(processor, 0);
    flushMessages();
    CHECK(processor.getLatencySamples() > 0);
}

void soundfont_echo_and_velocity_reach_the_engine()
{
    TempFolder folder;
    writeSf2(folder.dir.getChildFile("test.sf2"));
    RetroChipProcessor processor;
    loadSoundFont(processor, folder.dir, "test.sf2");

    const auto velocityPeak = [&](int velocity)
    {
        juce::MidiBuffer on;
        on.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(velocity)), 0);
        const auto peak = peakOf(processWith(processor, 6, on));
        allOff(processor);
        return peak;
    };
    const double hard = velocityPeak(127);
    const double soft = velocityPeak(30);
    CHECK(soft < hard * 0.7);
    setParam(processor, retrochip::ids::velocity, 0.0f);
    CHECK(std::abs(velocityPeak(30) - hard) < hard * 0.01);
    setParam(processor, retrochip::ids::velocity, 1.0f);

    // A short note's level after it has been cut: only the echo can still sound.
    const auto tail = [&]
    {
        juce::MidiBuffer on;
        on.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(120)), 0);
        processWith(processor, 4, on);
        juce::MidiBuffer off;
        off.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
        return peakOf(processWith(processor, 48, off), 48 * kBlock - 2048);
    };
    const double dry = tail();
    allOff(processor);
    CHECK(dry < 0.002);
    setParam(processor, retrochip::ids::sfEchoMix, 100.0f);
    setParam(processor, retrochip::ids::sfEchoDelay, 15.0f);
    setParam(processor, retrochip::ids::sfEchoFeedback, 70.0f);
    run(processor, 1);
    CHECK(tail() > dry + 0.005);
}

// With real threads, which is the case the atomics are for.
void replacing_soundfont_while_audio_runs()
{
    TempFolder folder;
    testsf2::TestSf2Options quiet;
    quiet.highAmplitude = 0.2;
    writeSf2(folder.dir.getChildFile("a.sf2"), quiet);
    writeBytes(folder.dir.getChildFile("b.sf2"), threeInstruments());

    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    setConsole(processor, kSoundFontConsole);
    const auto emptyState = saveState(processor); // the same settings with no file, as a host would send
    processor.selectSoundFont("a.sf2", -1, -1);
    processor.prepareToPlay(kSampleRate, kBlock);
    const auto withFile = stateNaming("b.sf2", 8, 5);

    std::atomic<bool> stop { false };
    std::atomic<int> blocks { 0 };
    std::atomic<bool> finite { true };
    std::thread audio([&]
    {
        juce::AudioBuffer<float> buffer(2, kBlock);
        for (int i = 0; !stop.load(); ++i)
        {
            juce::MidiBuffer midi;
            if (i % 2 == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 60 + i % 24, static_cast<juce::uint8>(100)), i % kBlock);
            processor.processBlock(buffer, midi);
            for (int s = 0; s < kBlock; ++s)
                if (!std::isfinite(buffer.getSample(0, s)))
                    finite = false;
            ++blocks;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    for (int i = 0; i < 200; ++i)
    {
        processor.selectSoundFont(i % 2 == 0 ? "b.sf2" : "a.sf2", -1, -1);
        if (i % 3 == 2)
            loadState(processor, emptyState); // a null is published: the case where a stale acknowledgement matters
        if (i % 7 == 0)
            loadState(processor, withFile);
        if (i % 11 == 0)
            processor.selectSoundFontInstrument(i % 3);
        if (i % 4 == 0)
            waitForLoad(processor);
        else
            juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        processor.releaseRetiredBanks();
        CHECK(processor.getRetiredBankCount() <= 4);
        if (i % 20 == 0)
            loadState(processor, saveState(processor));
    }
    processor.selectSoundFont("b.sf2", 0, 0);
    waitForLoad(processor);
    juce::Thread::sleep(20);
    stop = true;
    audio.join();

    CHECK(blocks.load() > 20);
    CHECK(finite.load());
    run(processor, 1);
    processor.releaseRetiredBanks();
    CHECK(processor.getRetiredBankCount() == 0);
    setConsole(processor, kSoundFontConsole);
    CHECK(noteLevel(processor) > 0.02);
}

// The loader must be stopped first, and must not need a message loop to stop.
void destruction_during_a_load_is_safe()
{
    TempFolder folder;
    writeBytes(folder.dir.getChildFile("three.sf2"), threeInstruments());

    for (int i = 0; i < 20; ++i)
    {
        RetroChipProcessor processor;
        processor.setLibraryRoot(folder.dir);
        processor.prepareToPlay(kSampleRate, kBlock);
        processor.selectSoundFont("three.sf2", -1, -1);
        if (i % 2 == 1)
            juce::Thread::sleep(5); // finished and waiting for a message loop that never runs, by now
    }

    // A bank taken by the audio thread, another waiting for it, a third retired: all go with the processor.
    RetroChipProcessor processor;
    loadSoundFont(processor, folder.dir, "three.sf2");
    run(processor, 2, 69);
    processor.selectSoundFont("three.sf2", -1, -1);
    waitForLoad(processor);
    CHECK(processor.getRetiredBankCount() == 1);
    processor.selectSoundFont("three.sf2", -1, -1);
}

void loader_delivers_the_last_request_on_its_own_thread()
{
    TempFolder folder;
    const auto good = folder.dir.getChildFile("good.sf2");
    writeBytes(good, threeInstruments());

    juce::CriticalSection lock;
    std::vector<SoundFontLoader::Result> results;
    juce::WaitableEvent arrived;
    juce::WaitableEvent entered;
    SoundFontLoader loader([&](SoundFontLoader::Result result)
    {
        entered.signal();
        const juce::ScopedLock scope(lock);
        results.push_back(std::move(result));
        arrived.signal();
    });

    const auto first = loader.request(good, kSampleRate, 8, 5);
    CHECK(arrived.wait(10000));
    {
        const juce::ScopedLock scope(lock);
        CHECK(results.size() == 1);
        if (results.size() == 1)
        {
            const auto& result = results.front();
            CHECK(result.id == first);
            CHECK(result.failure == SoundFontLoader::Failure::none);
            CHECK(result.bank != nullptr);
            CHECK(result.instruments.size() == 3);
            CHECK(result.instrument == 2);
            CHECK(result.instrumentFound);
        }
    }

    const auto second = loader.request(folder.dir.getChildFile("absent.sf2"), kSampleRate, 0, 0);
    CHECK(second != first);
    CHECK(arrived.wait(10000));
    {
        const juce::ScopedLock scope(lock);
        CHECK(results.size() == 2);
        if (results.size() == 2)
        {
            CHECK(results.back().id == second);
            CHECK(results.back().failure == SoundFontLoader::Failure::missing);
            CHECK(results.back().bank == nullptr);
        }
    }

    // A cancelled request is not heard of again. Holding the lock keeps the loader inside the callback of an earlier
    // result, so the request and the cancel both happen before it can look for more work.
    juce::uint64 third = 0;
    {
        const juce::ScopedLock scope(lock);
        entered.reset();
        third = loader.request(folder.dir.getChildFile("absent.sf2"), kSampleRate, 0, 0);
        CHECK(entered.wait(10000));
        loader.request(good, kSampleRate, 0, 0);
        loader.cancel();
    }
    CHECK(arrived.wait(10000)); // the earlier result, which was past cancelling
    CHECK(!arrived.wait(500));  // and nothing after it: the cancelled file takes milliseconds to load
    {
        const juce::ScopedLock scope(lock);
        CHECK(results.size() == 3);
        if (results.size() == 3)
            CHECK(results.back().id == third);
    }

    // The loader still answers the next request.
    const auto fourth = loader.request(good, kSampleRate, 0, 0);
    CHECK(arrived.wait(10000));
    const juce::ScopedLock scope(lock);
    CHECK(results.size() == 4);
    if (results.size() == 4)
    {
        CHECK(results.back().id == fourth);
        CHECK(results.back().bank != nullptr);
    }
}

// ---- The SoundFont panels ----

juce::ComboBox* comboById(juce::Component& editor, const char* id)
{
    auto* box = dynamic_cast<juce::ComboBox*>(findById(editor, id));
    CHECK(box != nullptr);
    return box;
}

juce::TextButton* textButtonById(juce::Component& editor, const char* id)
{
    auto* button = dynamic_cast<juce::TextButton*>(findById(editor, id));
    CHECK(button != nullptr);
    return button;
}

juce::Label* labelById(juce::Component& editor, const char* id)
{
    auto* label = dynamic_cast<juce::Label*>(findById(editor, id));
    CHECK(label != nullptr);
    return label;
}

juce::String statusText(juce::Component& editor)
{
    auto* label = labelById(editor, "sfStatus");
    return label != nullptr ? label->getText() : juce::String("(no status)");
}

void clickRefresh(juce::Component& editor)
{
    if (auto* button = textButtonById(editor, "sfRefresh"))
        button->onClick(); // triggerClick() is asynchronous
}

// The editor follows the processor on a 10 Hz timer.
void followProcessor() { juce::MessageManager::getInstance()->runDispatchLoopUntil(250); }

// Picks a menu entry as a click would, by its text. False if there is none.
bool choose(juce::ComboBox& box, const juce::String& text)
{
    for (int i = 0; i < box.getNumItems(); ++i)
    {
        if (box.getItemText(i) == text)
        {
            box.setSelectedItemIndex(i, juce::sendNotificationSync);
            return true;
        }
    }
    std::printf("  FAILED: no menu entry %s\n", text.toRawUTF8());
    ++failures;
    return false;
}

using Rows = std::vector<juce::String>;

// What the menu holds, in order: a group's heading as "# name", a file as its name.
Rows menuRows(juce::ComboBox& box)
{
    std::vector<juce::String> rows;
    juce::PopupMenu::MenuItemIterator iterator(*box.getRootMenu());
    while (iterator.next())
    {
        const auto& item = iterator.getItem();
        if (item.isSectionHeader)
            rows.push_back("# " + item.text);
        else if (!item.isSeparator)
            rows.push_back(item.text);
    }
    return rows;
}

Rows instrumentNames(juce::ComboBox& box)
{
    std::vector<juce::String> names;
    for (int i = 0; i < box.getNumItems(); ++i)
        names.push_back(box.getItemText(i));
    return names;
}

const char* const kSfOwn[] = { "sfFile", "sfInstrument", "sfOpenFolder", "sfRefresh", "sfStatus",
                               retrochip::ids::sfEchoMix, retrochip::ids::sfEchoDelay, retrochip::ids::sfEchoFeedback };

void soundfont_panel_shows_only_its_controls()
{
    TempFolder folder;
    writeSf2(folder.dir.getChildFile("test.sf2"));

    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    const auto visible = [&](const char* id)
    {
        auto* component = findById(*editor, id);
        CHECK(component != nullptr);
        return component != nullptr && component->isVisible();
    };

    const char* const own2a03[] = { retrochip::ids::channel, retrochip::ids::duty, retrochip::ids::noiseMode,
                                    retrochip::ids::attack, retrochip::ids::decay, retrochip::ids::sustain,
                                    retrochip::ids::release };
    const char* const ownYm[] = { retrochip::ids::genPatch, retrochip::ids::genAlgorithm, retrochip::ids::genFeedback,
                                  retrochip::ids::genBrightness, retrochip::ids::genAttack,
                                  retrochip::ids::genDecay, retrochip::ids::genRelease };
    const char* const ownSdsp[] = { retrochip::ids::snesInstrument, retrochip::ids::snesAttack,
                                    retrochip::ids::snesDecay, retrochip::ids::snesSustainLevel,
                                    retrochip::ids::snesSustainRate, retrochip::ids::snesRelease,
                                    retrochip::ids::snesEchoMix, retrochip::ids::snesEchoDelay,
                                    retrochip::ids::snesEchoFeedback, retrochip::ids::snesRootNote,
                                    retrochip::ids::snesLoop, "snesImport", "snesSampleStatus" };
    const char* const hidden[] = { retrochip::ids::vibratoRate, retrochip::ids::vibratoDepth, retrochip::ids::glide };
    const char* const shared[] = { retrochip::ids::velocity, retrochip::ids::gain, "console", "preset" };

    // None of the SoundFont's controls show on the other three consoles.
    for (int console = 0; console < 3; ++console)
    {
        setConsole(processor, console);
        for (const char* id : kSfOwn)
            CHECK(!visible(id));
        for (const char* id : hidden)
            CHECK(visible(id));
    }

    setConsole(processor, kSoundFontConsole);
    for (const char* id : kSfOwn)
        CHECK(visible(id));
    for (const char* id : shared)
        CHECK(visible(id));
    for (const char* id : hidden)
        CHECK(!visible(id));
    for (const char* id : own2a03)
        CHECK(!visible(id));
    for (const char* id : ownYm)
        CHECK(!visible(id));
    for (const char* id : ownSdsp)
        CHECK(!visible(id));
    for (const char* id : kSfOwn)
        checkEnabled(*editor, id, true);
    for (const char* id : shared)
        checkEnabled(*editor, id, true);
    checkTitles(*editor);
    checkLayout(*editor);

    // A 2A03 channel that greys out the shared controls must not do the same here.
    setConsole(processor, 0);
    setParam(processor, retrochip::ids::channel, 2.0f);
    checkEnabled(*editor, retrochip::ids::velocity, true);
    setConsole(processor, kSoundFontConsole);
    checkEnabled(*editor, retrochip::ids::velocity, true);
    checkEnabled(*editor, retrochip::ids::gain, true);

    // And the other consoles are as they were.
    setConsole(processor, 2);
    for (const char* id : ownSdsp)
        CHECK(visible(id));
    for (const char* id : kSfOwn)
        CHECK(!visible(id));
    for (const char* id : hidden)
        CHECK(visible(id));
    setConsole(processor, 0);
    for (const char* id : own2a03)
        CHECK(visible(id));
    checkLayout(*editor);
}

void console_selector_fits_every_segment()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto* selector = findById(*editor, "console");
    CHECK(selector != nullptr);
    if (selector == nullptr)
        return;

    // Each shown button's text fits.
    const auto checkFit = [&]
    {
        for (auto* child : selector->getChildren())
        {
            auto* button = dynamic_cast<juce::TextButton*>(child);
            if (button == nullptr || !button->isVisible())
                continue;
            const auto font = button->getLookAndFeel().getTextButtonFont(*button, button->getHeight());
            const int needed = juce::GlyphArrangement::getStringWidthInt(font, button->getButtonText()) + 12;
            if (needed > button->getWidth() || !selector->getLocalBounds().contains(button->getBounds()))
            {
                std::printf("  FAILED: %s does not fit its button (%d wide, %d needed)\n", button->getButtonText().toRawUTF8(),
                            button->getWidth(), needed);
                ++failures;
            }
        }
    };
    const auto button = [&](const char* id) { return dynamic_cast<juce::TextButton*>(findById(*editor, id)); };
    const auto shown = [&](const char* id) { return button(id) != nullptr && button(id)->isVisible(); };
    const auto on = [&](const char* id) { return button(id) != nullptr && button(id)->getToggleState(); };
    const auto press = [&](const char* id)
    {
        auto* b = button(id);
        CHECK(b != nullptr && b->onClick != nullptr);
        if (b != nullptr && b->onClick != nullptr)
            b->onClick();
    };
    const auto console = [&] { return juce::roundToInt(getParam(processor, kConsoleId)); };

    // Three tabs, one per console. The NES's has its chip and the two cartridge chips beside it.
    CHECK(button("tab_0") != nullptr && button("tab_0")->getButtonText() == "NES");
    CHECK(button("tab_1") != nullptr && button("tab_1")->getButtonText() == "Genesis");
    CHECK(button("tab_2") != nullptr && button("tab_2")->getButtonText() == "SNES");
    CHECK(on("tab_0") && on("console_0"));
    for (const char* id : { "console_1", "console_2", "console_3", "console_4" })
        CHECK(!shown(id));
    CHECK(shown("console_0") && shown("console_5") && shown("console_6"));
    CHECK(button("console_0")->getButtonText() == "2A03");
    CHECK(button("console_5")->getButtonText() == "VRC6");
    CHECK(button("console_6")->getButtonText() == "N163");
    checkFit();
    press("console_6");
    CHECK(console() == 6);
    press("tab_1");
    CHECK(!shown("console_5"));
    press("tab_0");
    CHECK(console() == 6);
    press("console_0");
    CHECK(console() == 0);

    // The Genesis tab has both of its chips beside it, and starts on the FM.
    press("tab_1");
    CHECK(console() == 1);
    CHECK(on("tab_1") && on("console_1"));
    CHECK(shown("console_1") && shown("console_4") && !shown("console_2") && !shown("console_3"));
    CHECK(button("console_1")->getButtonText() == "FM");
    CHECK(button("console_4")->getButtonText() == "PSG");
    checkFit();
    press("console_4");
    CHECK(console() == 4);
    CHECK(on("tab_1") && on("console_4") && !on("console_1"));

    // The SNES tab has its chip and the SoundFonts.
    press("tab_2");
    CHECK(console() == 2);
    CHECK(shown("console_2") && shown("console_3") && !shown("console_1") && !shown("console_4"));
    CHECK(button("console_2")->getButtonText() == "S-DSP");
    CHECK(button("console_3")->getButtonText() == "SoundFont");
    checkFit();
    press("console_3");
    CHECK(console() == 3);

    // A tab comes back to what it had.
    press("tab_1");
    CHECK(console() == 4);
    press("tab_2");
    CHECK(console() == 3);
    press("tab_0");
    CHECK(console() == 0);
    CHECK(!shown("console_3"));

    // The host's changes show too.
    setParam(processor, kConsoleId, 1.0f);
    CHECK(on("tab_1") && on("console_1") && !on("tab_0"));

    // Tooltips name the chips.
    const std::pair<const char*, const char*> tips[] = { { "tab_0", "2A03" }, { "console_1", "YM2612" }, { "console_4", "SN76489" },
                                                         { "console_2", "S-DSP" }, { "console_3", ".sf2" }, { "console_0", "2A03" },
                                                         { "console_5", "VRC6" }, { "console_6", "163" } };
    for (const auto& [id, chip] : tips)
        CHECK(button(id) != nullptr && button(id)->getTooltip().contains(chip));

    auto* preset = findById(*editor, "preset");
    CHECK(preset != nullptr && selector->getRight() <= preset->getX());
    checkTitles(*editor);

    // The preset menu's headings name the consoles and their chips.
    auto* box = dynamic_cast<juce::ComboBox*>(preset);
    juce::StringArray headings;
    if (box != nullptr)
        for (juce::PopupMenu::MenuItemIterator it(*box->getRootMenu()); it.next();)
            if (it.getItem().isSectionHeader)
                headings.add(it.getItem().text);
    CHECK(headings == juce::StringArray({ "NES", "Genesis FM", "SNES", "Genesis PSG", "NES VRC6", "NES N163" }));
}

void file_menu_has_a_heading_per_group()
{
    TempFolder folder;
    writeSf2(folder.dir.getChildFile("Zelda/Overworld.sf2"));
    writeSf2(folder.dir.getChildFile("Zelda/Dungeon.sf2"));
    writeSf2(folder.dir.getChildFile("Alpha/One.sf2"));
    writeSf2(folder.dir.getChildFile("Loose.sf2"));
    writeBytes(folder.dir.getChildFile("Alpha/notes.txt"), { 'x' });

    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    setConsole(processor, kSoundFontConsole);

    auto* box = comboById(*editor, "sfFile");
    if (box == nullptr)
        return;
    const Rows expected { "# Alpha", "One", "# Zelda", "Dungeon", "Overworld", "# Other", "Loose" };
    CHECK(menuRows(*box) == expected);
    CHECK(box->getNumItems() == 4);
    CHECK(box->getSelectedId() == 0); // nothing is chosen yet
    CHECK(processor.getSoundFontStatus().isEmpty());
    CHECK(statusText(*editor).isEmpty());

    // No folder, or none of the right files: an empty menu, and the status invites to add some.
    TempFolder emptyFolder;
    processor.setLibraryRoot(emptyFolder.dir);
    clickRefresh(*editor);
    CHECK(box->getNumItems() == 0);
    CHECK(statusText(*editor) == kNoSoundFont);
    CHECK(labelById(*editor, "sfStatus") != nullptr && labelById(*editor, "sfStatus")->getTooltip() == kNoSoundFont);

    processor.setLibraryRoot(emptyFolder.dir.getChildFile("absent"));
    clickRefresh(*editor);
    CHECK(box->getNumItems() == 0);
    CHECK(statusText(*editor) == kNoSoundFont);
    checkLayout(*editor);
}

void choosing_a_file_loads_it_and_lists_instruments()
{
    TempFolder folder;
    writeBytes(folder.dir.getChildFile("Group/three.sf2"), threeInstruments());
    testsf2::TestSf2Options options;
    options.presets = { { "Five", 2, 5 }, { "Seven", 2, 7 } };
    writeSf2(folder.dir.getChildFile("Group/no zero.sf2"), options);

    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    setConsole(processor, kSoundFontConsole);
    processor.prepareToPlay(kSampleRate, kBlock);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto* files = comboById(*editor, "sfFile");
    auto* instruments = comboById(*editor, "sfInstrument");
    auto* status = labelById(*editor, "sfStatus");
    if (files == nullptr || instruments == nullptr || status == nullptr)
        return;
    CHECK(instruments->getNumItems() == 0);

    if (choose(*files, "three"))
    {
        CHECK(processor.getSoundFontPath() == "Group/three.sf2");
        waitForLoad(processor);
        followProcessor();
        CHECK((instrumentNames(*instruments) == Rows { "Loud", "Soft", "Far" }));
        CHECK(instruments->getSelectedItemIndex() == 0);
        CHECK(files->getText() == "three");
        CHECK(status->getText().isEmpty());
        CHECK(noteLevel(processor) > 0.02);
    }

    // A font with no bank 0 program 0 opens on its first instrument without a word.
    if (choose(*files, "no zero"))
    {
        waitForLoad(processor);
        followProcessor();
        CHECK(processor.getSoundFontPath() == "Group/no zero.sf2");
        CHECK((instrumentNames(*instruments) == Rows { "Five", "Seven" }));
        CHECK(instruments->getSelectedItemIndex() == 0);
        CHECK(processor.getSoundFontStatus().isEmpty());
        CHECK(status->getText().isEmpty());
        CHECK(noteLevel(processor) > 0.02);
    }

    // An editor opened on a loaded plugin shows what plays.
    editor.reset(processor.createEditor());
    files = comboById(*editor, "sfFile");
    instruments = comboById(*editor, "sfInstrument");
    if (files != nullptr && instruments != nullptr)
    {
        CHECK(files->getText() == "no zero");
        CHECK((instrumentNames(*instruments) == Rows { "Five", "Seven" }));
        CHECK(instruments->getText() == "Five");
    }
}

void choosing_an_instrument_changes_the_sound()
{
    TempFolder folder;
    writeBytes(folder.dir.getChildFile("three.sf2"), threeInstruments());

    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    setConsole(processor, kSoundFontConsole);
    processor.prepareToPlay(kSampleRate, kBlock);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto* files = comboById(*editor, "sfFile");
    auto* instruments = comboById(*editor, "sfInstrument");
    if (files == nullptr || instruments == nullptr || !choose(*files, "three"))
        return;
    waitForLoad(processor);
    followProcessor();

    const double loud = noteLevel(processor);
    CHECK(loud > 0.02);

    if (choose(*instruments, "Soft"))
    {
        CHECK(processor.getSoundFontInstrumentIndex() == 1);
        const double soft = noteLevel(processor);
        CHECK(soft > 0.0005);
        CHECK(loud > soft * 4.0);
    }
    if (choose(*instruments, "Loud"))
    {
        CHECK(processor.getSoundFontInstrumentIndex() == 0);
        CHECK(std::abs(noteLevel(processor) - loud) < loud * 0.01);
    }

    // The menu follows a change made elsewhere.
    processor.selectSoundFontInstrument(2);
    followProcessor();
    CHECK(instruments->getSelectedItemIndex() == 2);
    CHECK(instruments->getText() == "Far");
    CHECK(instruments->getTooltip() == "Far");
}

void refresh_picks_up_new_files()
{
    TempFolder folder;
    writeSf2(folder.dir.getChildFile("Group/first.sf2"));

    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    setConsole(processor, kSoundFontConsole);
    auto* files = comboById(*editor, "sfFile");
    auto* refresh = textButtonById(*editor, "sfRefresh");
    if (files == nullptr || refresh == nullptr)
        return;
    CHECK(refresh->getButtonText() == "Refresh");
    CHECK(refresh->isEnabled() && refresh->getTitle().isNotEmpty());
    CHECK((menuRows(*files) == Rows { "# Group", "first" }));

    writeSf2(folder.dir.getChildFile("Group/second.sf2"));
    writeSf2(folder.dir.getChildFile("New/third.sf2"));
    CHECK(files->getNumItems() == 1); // the folder is read when asked, not on every tick
    followProcessor();
    CHECK(files->getNumItems() == 1);

    clickRefresh(*editor);
    CHECK((menuRows(*files) == Rows { "# Group", "first", "second", "# New", "third" }));

    // The file in use stays chosen across a refresh, and so does the choice of a file that has since gone.
    if (choose(*files, "second"))
    {
        waitForLoad(processor);
        followProcessor();
        clickRefresh(*editor);
        CHECK(files->getText() == "second");
        CHECK(files->getSelectedItemIndex() == 1);
    }
    folder.dir.getChildFile("Group/second.sf2").deleteFile();
    clickRefresh(*editor);
    CHECK((menuRows(*files) == Rows { "# Group", "first", "# New", "third" }));
    CHECK(files->getSelectedId() == 0);
    checkLayout(*editor);
}

void status_shows_missing_file()
{
    TempFolder folder;
    writeSf2(folder.dir.getChildFile("here.sf2"));
    writeBytes(folder.dir.getChildFile("broken.sf2"), { 'n', 'o', 't', ' ', 'a', ' ', 's', 'f', '2' });

    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    processor.prepareToPlay(kSampleRate, kBlock);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    setConsole(processor, kSoundFontConsole);
    auto* files = comboById(*editor, "sfFile");
    auto* status = labelById(*editor, "sfStatus");
    if (files == nullptr || status == nullptr)
        return;
    CHECK(status->getText().isEmpty());

    // A file that went between the scan and the click.
    writeSf2(folder.dir.getChildFile("gone.sf2"));
    clickRefresh(*editor);
    folder.dir.getChildFile("gone.sf2").deleteFile();
    if (choose(*files, "gone"))
    {
        waitForLoad(processor);
        followProcessor();
        CHECK(status->getText() == missingMessage("gone.sf2"));
        CHECK(status->getTooltip() == missingMessage("gone.sf2"));
    }

    // A file that cannot be read, and the message goes when a good one is chosen.
    if (choose(*files, "broken"))
    {
        waitForLoad(processor);
        followProcessor();
        CHECK(status->getText() == unreadableMessage("broken.sf2"));
    }
    if (choose(*files, "here"))
    {
        waitForLoad(processor);
        followProcessor();
        CHECK(status->getText().isEmpty());
        CHECK(status->getTooltip().isEmpty());
        auto* listed = comboById(*editor, "sfInstrument");
        CHECK(listed != nullptr && listed->getNumItems() == 1);
    }

    // The same for a project that names a file that is not there, and for an instrument that is not in the file.
    processor.selectSoundFont("Gone/away.sf2");
    waitForLoad(processor);
    followProcessor();
    CHECK(status->getText() == missingMessage("away.sf2"));
    processor.selectSoundFont("here.sf2", 5, 5);
    waitForLoad(processor);
    followProcessor();
    CHECK(status->getText() == kNoInstrument);
    CHECK(status->getTooltip() == kNoInstrument);
    checkLayout(*editor);
}

void refresh_reloads_a_file_that_came_back()
{
    TempFolder folder;
    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    processor.prepareToPlay(kSampleRate, kBlock);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    loadState(processor, stateNaming("gone.sf2", 8, 5, "Far"));
    waitForLoad(processor);
    setConsole(processor, kSoundFontConsole);
    followProcessor();
    auto* files = comboById(*editor, "sfFile");
    auto* instruments = comboById(*editor, "sfInstrument");
    auto* status = labelById(*editor, "sfStatus");
    if (files == nullptr || instruments == nullptr || status == nullptr)
        return;
    CHECK(status->getText() == missingMessage("gone.sf2"));
    CHECK(instruments->getNumItems() == 0);
    CHECK(noteLevel(processor) < 1e-6);

    // The user puts the file back and clicks Actualiser: the instrument is the one the project saved.
    writeBytes(folder.dir.getChildFile("gone.sf2"), threeInstruments());
    clickRefresh(*editor);
    waitForLoad(processor);
    followProcessor();
    CHECK(status->getText().isEmpty());
    CHECK(files->getText() == "gone");
    CHECK(instruments->getNumItems() == 3);
    CHECK(instruments->getSelectedItemIndex() == 2);
    CHECK(instruments->getText() == "Far");
    CHECK(noteLevel(processor) > 0.005);
    checkLayout(*editor);
}

void preset_menu_is_empty_on_soundfont()
{
    RetroChipProcessor processor;
    processor.setLibraryRoot(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("retrochip_no_such_folder"));
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto* box = comboById(*editor, "preset");
    if (box == nullptr)
        return;
    CHECK(box->getSelectedId() == 1);

    setConsole(processor, kSoundFontConsole);
    CHECK(box->getSelectedId() == 0);
    CHECK(box->getText().isEmpty());
    followProcessor();
    CHECK(box->getSelectedId() == 0);
    CHECK(box->isVisible() && box->isEnabled());

    // So a preset can be picked, which brings back its console.
    box->setSelectedId(25, juce::sendNotificationSync);
    CHECK(processor.getCurrentProgram() == 24);
    CHECK(processor.getActiveConsole() == Console::SDsp);
    CHECK(box->getSelectedId() == 25);

    setConsole(processor, kSoundFontConsole);
    processor.setCurrentProgram(3);
    followProcessor();
    CHECK(processor.getActiveConsole() == Console::Chip2A03);
    CHECK(box->getSelectedId() == 4);
}

void library_folder_is_created_for_the_button()
{
    TempFolder folder;

    // Absent, with parents that are absent too: made. Present: left alone, with what it holds.
    const auto root = folder.dir.getChildFile("RetroChip/SoundFonts");
    CHECK(!root.exists());
    CHECK(retrochip::createLibraryFolder(root).wasOk());
    CHECK(root.isDirectory());
    writeSf2(root.getChildFile("kept.sf2"));
    CHECK(retrochip::createLibraryFolder(root).wasOk());
    CHECK(root.getChildFile("kept.sf2").existsAsFile());

    // A file in the way, or no folder at all: it says so.
    const auto file = folder.dir.getChildFile("a file");
    file.replaceWithText("x");
    CHECK(retrochip::createLibraryFolder(file).failed());
    CHECK(retrochip::createLibraryFolder(juce::File()).failed());
    CHECK(file.existsAsFile());
}

void soundfont_snapshot()
{
    TempFolder folder;
    const juce::String longName = "A very long SoundFont name that cannot fit in the file menu of the plugin";
    writeBytes(folder.dir.getChildFile("A Collection With A Long Folder Name/" + longName + ".sf2"), threeInstruments());
    writeSf2(folder.dir.getChildFile("Short/small.sf2"));
    writeSf2(folder.dir.getChildFile("loose.sf2"));

    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    setConsole(processor, kSoundFontConsole);
    processor.prepareToPlay(kSampleRate, kBlock);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    CHECK(editor->getWidth() == 760);
    CHECK(editor->getHeight() == 520);
    checkTitles(*editor);
    checkLayout(*editor);

    auto* files = comboById(*editor, "sfFile");
    auto* instruments = comboById(*editor, "sfInstrument");
    if (files == nullptr || instruments == nullptr)
        return;

    // Nothing chosen yet; and with a folder that has no files, the invitation at its longest.
    saveSnapshot(*editor, "editor_soundfont_nothing_chosen.png");
    TempFolder emptyFolder;
    processor.setLibraryRoot(emptyFolder.dir);
    clickRefresh(*editor);
    CHECK(statusText(*editor) == kNoSoundFont);
    saveSnapshot(*editor, "editor_soundfont_empty.png");
    processor.setLibraryRoot(folder.dir);
    clickRefresh(*editor);

    // The long name stays readable through the tooltip, and so does that of an instrument.
    if (choose(*files, longName))
    {
        waitForLoad(processor);
        followProcessor();
        CHECK(files->getTooltip().contains(longName));
        CHECK(files->getTooltip().contains("A Collection With A Long Folder Name"));
        CHECK(files->getText() == longName);
        CHECK(instruments->getNumItems() == 3);
    }
    setParam(processor, retrochip::ids::sfEchoMix, 100.0f);
    setParam(processor, retrochip::ids::sfEchoDelay, 15.0f);
    setParam(processor, retrochip::ids::sfEchoFeedback, 100.0f);
    setParam(processor, retrochip::ids::gain, -36.0f);
    checkTitles(*editor);
    checkLayout(*editor);
    saveSnapshot(*editor, "editor_soundfont.png");

    // A message of the longest kind.
    processor.selectSoundFont("A Collection With A Long Folder Name/" + longName + ".sf2", 9, 9);
    waitForLoad(processor);
    followProcessor();
    CHECK(statusText(*editor) == kNoInstrument);
    checkLayout(*editor);
    saveSnapshot(*editor, "editor_soundfont_message.png");
}
} // namespace

using retrochip::GenesisImport;

// A pure tone: only operator 4 is heard, at full level.
void writeTfi(const juce::File& file, int algorithm = 7, int feedback = 2, int size = retrochip::kTfiSize)
{
    std::vector<juce::uint8> bytes(static_cast<size_t>(size), 0);
    if (size == retrochip::kTfiSize)
    {
        bytes[0] = static_cast<juce::uint8>(algorithm);
        bytes[1] = static_cast<juce::uint8>(feedback);
        for (size_t block = 0; block < 4; ++block)
        {
            auto* op = bytes.data() + 2 + 10 * block;
            op[0] = 1;                                // mul
            op[1] = 3;                                // no detune
            op[2] = block == 3 ? 0 : 127;             // tl
            op[4] = 31;                               // ar
            op[7] = 10;                               // rr
        }
    }
    file.replaceWithData(bytes.data(), bytes.size());
}

// The DAC holds each sample until the next, so the signal zigzags as it crosses zero. This counts only
// the crossings that come from well below it.
double pcmFrequency(const std::vector<float>& samples, double rate, size_t from)
{
    const auto threshold = static_cast<float>(-0.5 * peakOf(samples, from));
    std::vector<double> crossings;
    bool armed = false;
    for (size_t i = from; i < samples.size(); ++i)
    {
        if (samples[i] < threshold)
            armed = true;
        else if (armed && samples[i] >= 0.0f)
        {
            armed = false;
            crossings.push_back(static_cast<double>(i));
        }
    }
    if (crossings.size() < 3)
        return 0.0;
    return static_cast<double>(crossings.size() - 1) * rate / (crossings.back() - crossings.front());
}

void useGenesisImport(RetroChipProcessor& processor, int patch)
{
    setParam(processor, kConsoleId, 1.0f);
    setParam(processor, retrochip::ids::genPatch, static_cast<float>(patch));
    setParam(processor, retrochip::ids::genAlgorithm, 8.0f);
    setParam(processor, retrochip::ids::genRootNote, 69.0f);
    setParam(processor, retrochip::ids::vibratoDepth, 0.0f);
    setParam(processor, retrochip::ids::glide, 0.0f);
}

void genesis_timbre_menu_ends_with_the_imports()
{
    RetroChipProcessor processor;
    auto* choice = dynamic_cast<juce::AudioParameterChoice*>(processor.apvts.getParameter(retrochip::ids::genPatch));
    CHECK(choice != nullptr);
    if (choice == nullptr)
        return;
    CHECK(choice->choices.size() == 15);
    CHECK(choice->choices[GenesisEngine::kPcmKitPatch] == "My PCM kit");
    CHECK(choice->choices[GenesisEngine::kUserPatch] == "My TFI timbre");
    CHECK(choice->choices[GenesisEngine::kPcmPatch] == juce::String("My PCM sample"));
}

void tfi_import_plays_as_the_user_patch()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("Mon Timbre.tfi");
    writeTfi(file);

    RetroChipProcessor processor;
    useGenesisImport(processor, GenesisEngine::kUserPatch);
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(run(processor, 4, 69) == 0.0); // nothing imported yet
    allOff(processor);
    CHECK(processor.getGenesisStatus().contains(".tfi"));

    auto kind = GenesisImport::none;
    CHECK(processor.importGenesisFile(file, &kind).isEmpty());
    CHECK(kind == GenesisImport::patch);
    CHECK(processor.getUserPatchName() == "Mon Timbre");
    CHECK(processor.getGenesisStatus() == "Mon Timbre");
    const auto patch = processor.getUserPatch();
    CHECK(patch.has_value() && patch->feedback == 2);

    const auto played = playUserNote(processor, 69, 40);
    CHECK(peakOf(played) > 0.05);
    CHECK(std::abs(estimateFrequency(played, kSampleRate, 2 * kBlock) - 440.0) < 4.4);
}

void genesis_import_refuses_what_it_cannot_use()
{
    TempFolder folder;
    const auto good = folder.dir.getChildFile("good.tfi");
    writeTfi(good);
    const auto wrongSize = folder.dir.getChildFile("short.tfi");
    writeTfi(wrongSize, 7, 0, 41);
    const auto outOfRange = folder.dir.getChildFile("range.tfi");
    writeTfi(outOfRange, 9);
    const auto notAudio = folder.dir.getChildFile("text.wav");
    notAudio.replaceWithText("not a wav");
    const auto other = folder.dir.getChildFile("other.mp3");
    other.replaceWithText("x");
    const auto wrongCase = folder.dir.getChildFile("UPPER.TFI");
    writeTfi(wrongCase);

    RetroChipProcessor processor;
    useGenesisImport(processor, GenesisEngine::kUserPatch);
    CHECK(processor.importGenesisFile(good).isEmpty());

    for (const auto& file : { wrongSize, outOfRange, notAudio, other, folder.dir.getChildFile("missing.tfi") })
    {
        auto kind = GenesisImport::patch;
        const auto message = processor.importGenesisFile(file, &kind);
        CHECK(kind == GenesisImport::none);
        CHECK(message.contains(file.getFileName()));
        CHECK(processor.getGenesisStatus() == message);
        CHECK(processor.getUserPatchName() == "good"); // what was there stays
    }

    CHECK(processor.importGenesisFile(wrongCase).isEmpty());
    CHECK(processor.getUserPatchName() == "UPPER");
    CHECK(processor.getGenesisStatus() == "UPPER"); // the message is gone with the next import
}

void pcm_import_makes_8_bit_samples_at_the_dac_rate()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("hit.wav");

    // Half a second stays half a second, centred on the unsigned middle, with the level it had.
    writeMono(file, sineWave(440.0, 44100.0, 0.5, 0.5f), 44100.0);
    const auto result = retrochip::importPcm(file);
    CHECK(result.message.isEmpty());
    CHECK(result.name == "hit");
    CHECK(std::abs(static_cast<double>(result.pcm.size()) - 0.5 * GenesisEngine::kPcmRate) < 2.0);
    if (result.pcm.empty())
        return;
    const auto [low, high] = std::minmax_element(result.pcm.begin(), result.pcm.end());
    CHECK(std::abs(*high - (128 + 63)) <= 4);
    CHECK(std::abs(*low - (128 - 63)) <= 4);

    // What is over full scale comes down. A byte never leaves the DAC's symmetrical range.
    writeMono(file, sineWave(440.0, 32000.0, 0.2, 1.5f), 32000.0, 32);
    const auto limited = retrochip::importPcm(file);
    if (limited.pcm.empty())
    {
        CHECK(false);
        return;
    }
    const auto [lowest, highest] = std::minmax_element(limited.pcm.begin(), limited.pcm.end());
    CHECK(*highest <= 255 && *highest >= 250);
    CHECK(*lowest >= 1 && *lowest <= 6);

    // A stereo file is mixed: opposite channels cancel.
    const auto left = sineWave(440.0, 32000.0, 0.2);
    auto right = left;
    for (auto& s : right)
        s = -s;
    writeWav(file, { left, right }, 32000.0, 16);
    const auto mixed = retrochip::importPcm(file);
    CHECK(!mixed.pcm.empty());
    for (const auto byte : mixed.pcm)
        CHECK(std::abs(byte - 128) <= 1);
}

void pcm_import_cuts_what_is_too_long()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("long.wav");
    writeMono(file, sineWave(220.0, 8000.0, 22.0), 8000.0);

    const auto result = retrochip::importPcm(file);
    CHECK(static_cast<int>(result.pcm.size()) == retrochip::kMaxPcmBytes);
    CHECK(result.truncated);
    CHECK(result.message.contains("256 KB"));

    // A cut file is still an import, and says so.
    RetroChipProcessor processor;
    auto kind = GenesisImport::none;
    CHECK(processor.importGenesisFile(file, &kind).contains("256 KB"));
    CHECK(kind == GenesisImport::pcm);
    CHECK(processor.getGenesisStatus().contains("256 KB"));
}

void pcm_import_plays_on_the_root_note()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("tone.wav");
    writeMono(file, sineWave(440.0, 32000.0, 1.0));

    RetroChipProcessor processor;
    useGenesisImport(processor, GenesisEngine::kPcmPatch);
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(run(processor, 4, 69) == 0.0);
    allOff(processor);
    CHECK(processor.getGenesisStatus().contains("WAV"));

    auto kind = GenesisImport::none;
    CHECK(processor.importGenesisFile(file, &kind).isEmpty());
    CHECK(kind == GenesisImport::pcm);
    CHECK(processor.getUserPcmName() == "tone");
    CHECK(processor.getGenesisStatus() == "tone");

    const auto played = playUserNote(processor, 69, 40);
    CHECK(peakOf(played) > 0.05);
    CHECK(std::abs(pcmFrequency(played, kSampleRate, 2 * kBlock) - 440.0) < 4.4);
    setParam(processor, retrochip::ids::genRootNote, 57.0f);
    CHECK(std::abs(pcmFrequency(playUserNote(processor, 69, 40), kSampleRate, 2 * kBlock) - 880.0) < 8.8);
}

void pcm_loop_parameter_reaches_the_engine()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("short.wav");
    writeMono(file, sineWave(440.0, 32000.0, 0.1));

    RetroChipProcessor processor;
    useGenesisImport(processor, GenesisEngine::kPcmPatch);
    processor.importGenesisFile(file);

    const auto lateLevel = [&]
    {
        const auto played = playUserNote(processor, 69, 60); // over half a second
        return peakOf(played, played.size() - 2 * static_cast<size_t>(kBlock));
    };
    CHECK(lateLevel() < 1e-2);
    setParam(processor, retrochip::ids::genLoop, 1.0f);
    CHECK(lateLevel() > 0.05);
}

void each_genesis_import_keeps_the_other()
{
    TempFolder folder;
    const auto tfi = folder.dir.getChildFile("timbre.tfi");
    writeTfi(tfi);
    const auto wav = folder.dir.getChildFile("hit.wav");
    writeMono(wav, sineWave(440.0, 32000.0, 0.2));

    RetroChipProcessor processor;
    processor.importGenesisFile(tfi);
    processor.importGenesisFile(wav);
    CHECK(processor.getUserPatchName() == "timbre");
    CHECK(processor.getUserPcmName() == "hit");
    processor.importGenesisFile(tfi);
    CHECK(processor.getUserPcmName() == "hit");
}

void genesis_imports_round_trip_in_the_state()
{
    TempFolder folder;
    const auto tfi = folder.dir.getChildFile("timbre.tfi");
    writeTfi(tfi, 7, 3);
    const auto wav = folder.dir.getChildFile("hit.wav");
    writeMono(wav, sineWave(440.0, 32000.0, 0.5));

    juce::MemoryBlock saved;
    {
        RetroChipProcessor processor;
        useGenesisImport(processor, GenesisEngine::kPcmPatch);
        setParam(processor, retrochip::ids::genRootNote, 57.0f);
        setParam(processor, retrochip::ids::genLoop, 1.0f);
        processor.importGenesisFile(tfi);
        processor.importGenesisFile(wav);
        processor.getStateInformation(saved);
    }

    // The files are gone: the state has its own copy.
    folder.dir.deleteRecursively();

    RetroChipProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    CHECK(restored.getUserPatchName() == "timbre");
    CHECK(restored.getUserPcmName() == "hit");
    const auto patch = restored.getUserPatch();
    CHECK(patch.has_value() && patch->feedback == 3);
    CHECK(getParam(restored, retrochip::ids::genRootNote) == 57.0f);
    CHECK(getParam(restored, retrochip::ids::genLoop) == 1.0f);
    const auto played = playUserNote(restored, 69, 40);
    CHECK(peakOf(played) > 0.05);
    CHECK(std::abs(pcmFrequency(played, kSampleRate, 2 * kBlock) - 880.0) < 8.8);

    setParam(restored, retrochip::ids::genPatch, static_cast<float>(GenesisEngine::kUserPatch));
    CHECK(peakOf(playUserNote(restored, 69, 40)) > 0.05);

    // They don't stay among the parameters, where they would come back after being replaced.
    CHECK(!restored.apvts.state.hasProperty(retrochip::kGenesisPcmProperty));
    CHECK(!restored.apvts.state.hasProperty(retrochip::kGenesisTfiProperty));

    // A state without them takes them away.
    RetroChipProcessor blank;
    juce::MemoryBlock nothing;
    blank.getStateInformation(nothing);
    restored.setStateInformation(nothing.getData(), static_cast<int>(nothing.getSize()));
    CHECK(restored.getUserPatchName().isEmpty());
    CHECK(restored.getUserPcmName().isEmpty());
}

void damaged_genesis_state_is_silent_with_a_message()
{
    TempFolder folder;
    const auto tfi = folder.dir.getChildFile("timbre.tfi");
    writeTfi(tfi);

    RetroChipProcessor processor;
    useGenesisImport(processor, GenesisEngine::kUserPatch);
    processor.importGenesisFile(tfi);
    juce::MemoryBlock saved;
    processor.getStateInformation(saved);

    auto xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
    CHECK(xml != nullptr);
    if (xml == nullptr)
        return;
    xml->setAttribute(retrochip::kGenesisTfiProperty, "AAAA");
    xml->setAttribute(retrochip::kGenesisPcmProperty, "not base 64 !");
    juce::MemoryBlock damaged;
    juce::AudioProcessor::copyXmlToBinary(*xml, damaged);

    RetroChipProcessor restored;
    restored.setStateInformation(damaged.getData(), static_cast<int>(damaged.getSize()));
    CHECK(restored.getUserPatchName().isEmpty());
    CHECK(restored.getUserPcmName().isEmpty());
    CHECK(restored.getGenesisStatus().containsIgnoreCase("unreadable"));
    CHECK(peakOf(playUserNote(restored, 69, 8)) == 0.0);
}

void replaced_genesis_sounds_wait_for_the_audio_thread()
{
    TempFolder folder;
    const auto tfi = folder.dir.getChildFile("timbre.tfi");
    writeTfi(tfi);

    RetroChipProcessor processor;
    useGenesisImport(processor, GenesisEngine::kUserPatch);
    processor.prepareToPlay(kSampleRate, kBlock);

    // With no audio running, nothing piles up.
    for (int i = 0; i < 4; ++i)
        processor.importGenesisFile(tfi);
    CHECK(processor.getRetiredSoundCount() == 0);

    // Once the audio thread has one, it stays until the audio thread has moved on.
    processWith(processor, 1);
    processor.importGenesisFile(tfi);
    CHECK(processor.getRetiredSoundCount() == 1);
    processor.releaseRetiredSamples();
    CHECK(processor.getRetiredSoundCount() == 1);
    processWith(processor, 1);
    processor.releaseRetiredSamples();
    CHECK(processor.getRetiredSoundCount() == 0);
}

void replacing_genesis_sound_while_audio_runs()
{
    TempFolder folder;
    const auto tfi = folder.dir.getChildFile("timbre.tfi");
    writeTfi(tfi);
    const auto wav = folder.dir.getChildFile("hit.wav");
    writeMono(wav, sineWave(440.0, 32000.0, 0.05));

    RetroChipProcessor processor;
    useGenesisImport(processor, GenesisEngine::kPcmPatch);
    processor.prepareToPlay(kSampleRate, kBlock);

    std::atomic<bool> stop { false };
    std::atomic<bool> finite { true };
    std::thread audio([&]
    {
        juce::AudioBuffer<float> buffer(2, kBlock);
        for (int i = 0; !stop.load(); ++i)
        {
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 60 + i % 24, static_cast<juce::uint8>(100)), i % kBlock);
            processor.processBlock(buffer, midi);
            for (int s = 0; s < kBlock; ++s)
                if (!std::isfinite(buffer.getSample(0, s)))
                    finite = false;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    juce::MemoryBlock nothing;
    {
        RetroChipProcessor blank;
        blank.getStateInformation(nothing);
    }
    juce::Array<juce::File> kitFiles;
    kitFiles.add(wav);
    for (int i = 0; i < 100; ++i)
    {
        processor.importGenesisFile(i % 2 == 0 ? wav : tfi);
        processor.importGenesisKit(kitFiles);
        const int timbres[] = { GenesisEngine::kUserPatch, GenesisEngine::kPcmPatch, GenesisEngine::kPcmKitPatch };
        setParam(processor, retrochip::ids::genPatch, static_cast<float>(timbres[i % 3]));
        if (i % 7 == 6)
            processor.setStateInformation(nothing.getData(), static_cast<int>(nothing.getSize())); // publishes a null
        setParam(processor, kConsoleId, 1.0f);
        processor.releaseRetiredSamples();
    }
    stop = true;
    audio.join();
    CHECK(finite.load());
}

void editor_genesis_import_selects_what_was_imported()
{
    TempFolder folder;
    const auto tfi = folder.dir.getChildFile("timbre.tfi");
    writeTfi(tfi, 4, 6);
    const auto wav = folder.dir.getChildFile("hit.wav");
    writeMono(wav, sineWave(440.0, 32000.0, 0.2));
    const auto bad = folder.dir.getChildFile("bad.txt");
    bad.replaceWithText("x");

    RetroChipProcessor processor;
    setParam(processor, kConsoleId, 1.0f);
    setParam(processor, retrochip::ids::genPatch, 3.0f);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto* concrete = dynamic_cast<retrochip::RetroChipEditor*>(editor.get());
    auto* box = dynamic_cast<juce::ComboBox*>(findById(*editor, retrochip::ids::genPatch));
    CHECK(concrete != nullptr && box != nullptr);
    if (concrete == nullptr || box == nullptr)
        return;
    CHECK(box->getNumItems() == 15);

    concrete->importGenesisFile(bad);
    CHECK(getParam(processor, retrochip::ids::genPatch) == 3.0f);

    // A timbre brings along its algorithm and feedback, as those of the menu do.
    concrete->importGenesisFile(tfi);
    CHECK(getParam(processor, retrochip::ids::genPatch) == static_cast<float>(GenesisEngine::kUserPatch));
    CHECK(getParam(processor, retrochip::ids::genAlgorithm) == 5.0f);
    CHECK(getParam(processor, retrochip::ids::genFeedback) == 6.0f);
    CHECK(box->getSelectedItemIndex() == GenesisEngine::kUserPatch);

    concrete->importGenesisFile(wav);
    CHECK(getParam(processor, retrochip::ids::genPatch) == static_cast<float>(GenesisEngine::kPcmPatch));
    CHECK(box->getSelectedItemIndex() == GenesisEngine::kPcmPatch);

    // Picking the timbre in the menu brings them back too.
    setParam(processor, retrochip::ids::genAlgorithm, 1.0f);
    box->setSelectedItemIndex(GenesisEngine::kUserPatch, juce::sendNotificationSync);
    CHECK(getParam(processor, retrochip::ids::genAlgorithm) == 5.0f);
    CHECK(getParam(processor, retrochip::ids::genFeedback) == 6.0f);

    // Without a timbre imported, the menu's choice leaves them alone.
    RetroChipProcessor empty;
    setParam(empty, kConsoleId, 1.0f);
    setParam(empty, retrochip::ids::genAlgorithm, 2.0f);
    std::unique_ptr<juce::AudioProcessorEditor> emptyEditor(empty.createEditor());
    auto* emptyBox = dynamic_cast<juce::ComboBox*>(findById(*emptyEditor, retrochip::ids::genPatch));
    CHECK(emptyBox != nullptr);
    if (emptyBox == nullptr)
        return;
    emptyBox->setSelectedItemIndex(GenesisEngine::kUserPatch, juce::sendNotificationSync);
    CHECK(getParam(empty, retrochip::ids::genPatch) == static_cast<float>(GenesisEngine::kUserPatch));
    CHECK(getParam(empty, retrochip::ids::genAlgorithm) == 2.0f);
}

void ym2612_panel_follows_the_timbre()
{
    TempFolder folder;
    const auto wav = folder.dir.getChildFile("Un nom assez long pour le panneau.wav");
    writeMono(wav, sineWave(440.0, 32000.0, 0.2));

    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    const auto visible = [&](const char* id)
    {
        auto* component = findById(*editor, id);
        CHECK(component != nullptr);
        return component != nullptr && component->isVisible();
    };

    // The import is on this console only.
    CHECK(!visible("genImport"));
    CHECK(!visible("genStatus"));
    CHECK(!visible(retrochip::ids::genRootNote));
    CHECK(!visible(retrochip::ids::genLoop));

    setParam(processor, kConsoleId, 1.0f);
    CHECK(visible("genImport"));
    CHECK(visible("genStatus"));
    CHECK(visible(retrochip::ids::genAlgorithm));
    CHECK(!visible(retrochip::ids::genRootNote));
    CHECK(!visible(retrochip::ids::genLoop));
    checkEnabled(*editor, retrochip::ids::genFeedback, true);
    checkLayout(*editor);

    // The chip's FM settings do nothing to a sample, and the sample has its own.
    setParam(processor, retrochip::ids::genPatch, static_cast<float>(GenesisEngine::kPcmPatch));
    CHECK(!visible(retrochip::ids::genAlgorithm));
    CHECK(visible(retrochip::ids::genRootNote));
    CHECK(visible(retrochip::ids::genLoop));
    for (const char* id : { retrochip::ids::genFeedback, retrochip::ids::genBrightness, retrochip::ids::genAttack, retrochip::ids::genDecay })
        checkEnabled(*editor, id, false);
    checkEnabled(*editor, retrochip::ids::genRelease, true);
    checkEnabled(*editor, retrochip::ids::vibratoDepth, true);
    checkLayout(*editor);

    auto* label = dynamic_cast<juce::Label*>(findById(*editor, "genStatus"));
    CHECK(label != nullptr && label->getText().contains("WAV"));
    processor.importGenesisFile(wav);
    CHECK(label != nullptr && label->getText() == wav.getFileNameWithoutExtension());
    CHECK(label != nullptr && label->getTooltip() == wav.getFileNameWithoutExtension());
    checkTitles(*editor);
    saveSnapshot(*editor, "editor_ym2612_pcm.png");

    setParam(processor, retrochip::ids::genPatch, 2.0f);
    CHECK(visible(retrochip::ids::genAlgorithm));
    CHECK(!visible(retrochip::ids::genRootNote));
    checkEnabled(*editor, retrochip::ids::genFeedback, true);

    // Leaving the console takes the sample's controls with it, whatever the timbre.
    setParam(processor, retrochip::ids::genPatch, static_cast<float>(GenesisEngine::kPcmPatch));
    setParam(processor, kConsoleId, 0.0f);
    CHECK(!visible(retrochip::ids::genRootNote));
    CHECK(!visible(retrochip::ids::genLoop));
    CHECK(!visible(retrochip::ids::genAlgorithm));
    checkEnabled(*editor, retrochip::ids::vibratoDepth, true);
}

juce::Array<juce::File> writeDrumFiles(const juce::File& folder, std::initializer_list<const char*> names, double seconds = 0.2)
{
    juce::Array<juce::File> files;
    double frequency = 200.0;
    for (const auto* name : names)
    {
        const auto file = folder.getChildFile(name);
        writeMono(file, sineWave(frequency, 32000.0, seconds));
        files.add(file);
        frequency += 100.0;
    }
    return files;
}

void kit_import_puts_the_files_on_the_keys_in_name_order()
{
    TempFolder folder;
    auto files = writeDrumFiles(folder.dir, { "drum10.wav", "drum2.wav", "Drum1.wav" });

    const auto kit = retrochip::importKit(files);
    CHECK(kit.message.isEmpty());
    CHECK(kit.samples.size() == 3);
    if (kit.samples.size() != 3)
        return;

    // In the order a person would read them, from the keyboard's lowest key up.
    CHECK(kit.samples[0].name == "Drum1" && kit.samples[0].note == retrochip::kFirstKitNote);
    CHECK(kit.samples[1].name == "drum2" && kit.samples[1].note == retrochip::kFirstKitNote + 1);
    CHECK(kit.samples[2].name == "drum10" && kit.samples[2].note == retrochip::kFirstKitNote + 2);
    for (const auto& sample : kit.samples)
        CHECK(std::abs(static_cast<double>(sample.pcm.size()) - 0.2 * GenesisEngine::kPcmRate) < 2.0);
}

void kit_import_skips_what_it_cannot_use()
{
    TempFolder folder;
    auto files = writeDrumFiles(folder.dir, { "a.wav", "c.wav" });
    const auto text = folder.dir.getChildFile("b.wav");
    text.replaceWithText("not a wav");
    files.add(text);
    const auto other = folder.dir.getChildFile("d.tfi");
    writeTfi(other);
    files.add(other);

    const auto kit = retrochip::importKit(files);
    CHECK(kit.samples.size() == 2);
    if (kit.samples.size() == 2)
    {
        CHECK(kit.samples[1].name == "c");
        CHECK(kit.samples[1].note == retrochip::kFirstKitNote + 1); // no gap where the bad file was
    }
    CHECK(kit.message.contains("b.wav"));
    CHECK(kit.message.contains("d.tfi"));

    juce::Array<juce::File> onlyBad;
    onlyBad.add(text);
    const auto nothing = retrochip::importKit(onlyBad);
    CHECK(nothing.samples.empty());
    CHECK(nothing.message.contains("b.wav"));
    CHECK(retrochip::importKit({}).samples.empty());
}

void kit_import_keeps_to_its_limits()
{
    TempFolder folder;

    // The total is what is limited: the file that reaches it is cut, and those after it have no room.
    juce::Array<juce::File> long_;
    for (const auto* name : { "a.wav", "b.wav", "c.wav" })
    {
        const auto file = folder.dir.getChildFile(name);
        writeMono(file, sineWave(220.0, 8000.0, 12.0), 8000.0);
        long_.add(file);
    }
    const auto cut = retrochip::importKit(long_);
    size_t total = 0;
    for (const auto& sample : cut.samples)
        total += sample.pcm.size();
    CHECK(total == static_cast<size_t>(retrochip::kMaxPcmBytes));
    CHECK(cut.samples.size() == 2);
    CHECK(cut.message.contains("256 KB"));

    // There are no more samples than keys on the keyboard.
    juce::Array<juce::File> many;
    for (int i = 0; i < retrochip::kMaxKitSamples + 3; ++i)
    {
        const auto file = folder.dir.getChildFile("m" + juce::String(i) + ".wav");
        writeMono(file, sineWave(220.0, 8000.0, 0.01), 8000.0);
        many.add(file);
    }
    const auto full = retrochip::importKit(many);
    CHECK(static_cast<int>(full.samples.size()) == retrochip::kMaxKitSamples);
    if (full.samples.empty())
        return;
    CHECK(full.samples.back().note == retrochip::kFirstKitNote + retrochip::kMaxKitSamples - 1);
    CHECK(full.samples.back().note <= 96);
    CHECK(full.message.contains(juce::String(retrochip::kMaxKitSamples)));
}

void kit_plays_through_the_processor()
{
    TempFolder folder;
    const auto files = writeDrumFiles(folder.dir, { "kick.wav", "snare.wav" }, 1.0);

    RetroChipProcessor processor;
    useGenesisImport(processor, GenesisEngine::kPcmKitPatch);
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(run(processor, 4, retrochip::kFirstKitNote) == 0.0);
    allOff(processor);
    CHECK(processor.getGenesisStatus().contains("WAV"));

    bool imported = false;
    CHECK(processor.importGenesisKit(files, &imported).isEmpty());
    CHECK(imported);
    CHECK(processor.getGenesisStatus().contains("2"));
    CHECK(processor.getKitDescription().contains("kick"));
    CHECK(processor.getKitDescription().contains("snare"));
    CHECK(processor.getKitDescription().contains("C1"));

    const auto kick = playUserNote(processor, retrochip::kFirstKitNote, 40);
    const auto snare = playUserNote(processor, retrochip::kFirstKitNote + 1, 40);
    CHECK(peakOf(kick) > 0.05);
    CHECK(std::abs(pcmFrequency(kick, kSampleRate, 2 * kBlock) - 200.0) < 3.0);
    CHECK(std::abs(pcmFrequency(snare, kSampleRate, 2 * kBlock) - 300.0) < 4.0);
    CHECK(peakOf(playUserNote(processor, retrochip::kFirstKitNote + 2, 8)) == 0.0);

    // A kit that cannot be read leaves the one in place.
    juce::Array<juce::File> bad;
    bad.add(folder.dir.getChildFile("missing.wav"));
    imported = true;
    CHECK(processor.importGenesisKit(bad, &imported).contains("missing.wav"));
    CHECK(!imported);
    CHECK(peakOf(playUserNote(processor, retrochip::kFirstKitNote, 40)) > 0.05);
}

void kit_round_trips_in_the_state_and_keeps_the_other_imports()
{
    TempFolder folder;
    const auto files = writeDrumFiles(folder.dir, { "kick.wav", "snare.wav" }, 1.0);
    const auto tfi = folder.dir.getChildFile("timbre.tfi");
    writeTfi(tfi);
    const auto wav = folder.dir.getChildFile("tone.wav");
    writeMono(wav, sineWave(440.0, 32000.0, 0.2));

    juce::MemoryBlock saved;
    {
        RetroChipProcessor processor;
        useGenesisImport(processor, GenesisEngine::kPcmKitPatch);
        processor.importGenesisFile(tfi);
        processor.importGenesisFile(wav);
        processor.importGenesisKit(files);
        CHECK(processor.getUserPatchName() == "timbre");
        CHECK(processor.getUserPcmName() == "tone");
        processor.importGenesisFile(wav);
        CHECK(processor.getKitDescription().contains("snare"));
        processor.getStateInformation(saved);
    }
    folder.dir.deleteRecursively();

    RetroChipProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    CHECK(restored.getKitDescription().contains("kick"));
    CHECK(restored.getKitDescription().contains("snare"));
    CHECK(restored.getUserPatchName() == "timbre");
    CHECK(restored.getUserPcmName() == "tone");
    const auto snare = playUserNote(restored, retrochip::kFirstKitNote + 1, 40);
    CHECK(peakOf(snare) > 0.05);
    CHECK(std::abs(pcmFrequency(snare, kSampleRate, 2 * kBlock) - 300.0) < 4.0);

    // The kit's copy doesn't stay among the parameters, and a second save has it once.
    CHECK(!restored.apvts.state.getChildWithName(retrochip::kGenesisKitTree).isValid());
    juce::MemoryBlock again;
    restored.getStateInformation(again);
    auto xml = juce::AudioProcessor::getXmlFromBinary(again.getData(), static_cast<int>(again.getSize()));
    CHECK(xml != nullptr && xml->getNumChildElements() > 0);
    if (xml != nullptr)
    {
        auto* kit = xml->getChildByName(retrochip::kGenesisKitTree);
        CHECK(kit != nullptr && kit->getNumChildElements() == 2);
    }

    // A state without one takes it away.
    RetroChipProcessor blank;
    juce::MemoryBlock nothing;
    blank.getStateInformation(nothing);
    restored.setStateInformation(nothing.getData(), static_cast<int>(nothing.getSize()));
    CHECK(restored.getKitDescription().isEmpty());
}

void damaged_kit_in_the_state_keeps_what_it_can()
{
    TempFolder folder;
    const auto files = writeDrumFiles(folder.dir, { "kick.wav", "snare.wav", "hat.wav" });

    RetroChipProcessor processor;
    useGenesisImport(processor, GenesisEngine::kPcmKitPatch);
    processor.importGenesisKit(files);
    juce::MemoryBlock saved;
    processor.getStateInformation(saved);

    const auto loadWith = [&](const std::function<void(juce::XmlElement&)>& damage)
    {
        auto xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
        auto* kit = xml != nullptr ? xml->getChildByName(retrochip::kGenesisKitTree) : nullptr;
        CHECK(kit != nullptr);
        if (kit != nullptr)
            damage(*kit);
        juce::MemoryBlock damaged;
        if (xml != nullptr)
            juce::AudioProcessor::copyXmlToBinary(*xml, damaged);
        auto restored = std::make_unique<RetroChipProcessor>();
        restored->setStateInformation(damaged.getData(), static_cast<int>(damaged.getSize()));
        return restored;
    };

    // Bad data, a note off the keyboard's range, and the same note twice each lose one sample and say so.
    const std::function<void(juce::XmlElement&)> damages[] = {
        [](juce::XmlElement& kit) { kit.getChildElement(1)->setAttribute(retrochip::kGenesisKitDataProperty, "!!"); },
        [](juce::XmlElement& kit) { kit.getChildElement(1)->setAttribute(retrochip::kGenesisKitNoteProperty, 200); },
        [](juce::XmlElement& kit) { kit.getChildElement(1)->setAttribute(retrochip::kGenesisKitNoteProperty, kit.getChildElement(0)->getIntAttribute(retrochip::kGenesisKitNoteProperty)); },
    };
    for (const auto& damage : damages)
    {
        auto restored = loadWith(damage);
        CHECK(restored->getKitDescription().contains("hat"));
        CHECK(!restored->getKitDescription().contains("kick")); // the second, in name order
        CHECK(restored->getKitDescription().contains("snare"));
        CHECK(restored->getGenesisStatus().containsIgnoreCase("unreadable"));
    }
}

void editor_import_picks_the_kit_from_the_selection()
{
    TempFolder folder;
    const auto files = writeDrumFiles(folder.dir, { "kick.wav", "snare.wav" });
    const auto single = folder.dir.getChildFile("tone.wav");
    writeMono(single, sineWave(440.0, 32000.0, 0.2));
    const auto tfi = folder.dir.getChildFile("timbre.tfi");
    writeTfi(tfi);

    RetroChipProcessor processor;
    setParam(processor, kConsoleId, 1.0f);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto* concrete = dynamic_cast<retrochip::RetroChipEditor*>(editor.get());
    CHECK(concrete != nullptr);
    if (concrete == nullptr)
        return;
    const auto timbre = [&] { return juce::roundToInt(getParam(processor, retrochip::ids::genPatch)); };

    // One file is the instrument for the whole keyboard, several are a kit.
    concrete->importGenesisFile(single);
    CHECK(timbre() == GenesisEngine::kPcmPatch);
    concrete->importGenesisFiles(files);
    CHECK(timbre() == GenesisEngine::kPcmKitPatch);
    CHECK(processor.getUserPcmName() == "tone");

    // With the kit as the timbre, one WAV is a kit of one, and a timbre file is still a timbre.
    concrete->importGenesisFile(single);
    CHECK(timbre() == GenesisEngine::kPcmKitPatch);
    CHECK(processor.getKitDescription().contains("tone"));
    CHECK(!processor.getKitDescription().contains("kick"));
    concrete->importGenesisFile(tfi);
    CHECK(timbre() == GenesisEngine::kUserPatch);
}

void ym2612_panel_with_the_kit()
{
    TempFolder folder;
    const auto files = writeDrumFiles(folder.dir, { "kick.wav", "snare.wav", "hat.wav" });

    RetroChipProcessor processor;
    setParam(processor, kConsoleId, 1.0f);
    setParam(processor, retrochip::ids::genPatch, static_cast<float>(GenesisEngine::kPcmKitPatch));
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    const auto visible = [&](const char* id)
    {
        auto* component = findById(*editor, id);
        CHECK(component != nullptr);
        return component != nullptr && component->isVisible();
    };

    CHECK(!visible(retrochip::ids::genAlgorithm));
    CHECK(visible(retrochip::ids::genRootNote));
    CHECK(visible(retrochip::ids::genLoop));

    // A kit plays each sample as it is, so nearly nothing applies to it.
    for (const char* id : { retrochip::ids::genFeedback, retrochip::ids::genBrightness, retrochip::ids::genAttack, retrochip::ids::genDecay,
                            retrochip::ids::genRelease, retrochip::ids::vibratoRate, retrochip::ids::vibratoDepth, retrochip::ids::glide,
                            retrochip::ids::genRootNote, retrochip::ids::genLoop })
        checkEnabled(*editor, id, false);
    checkEnabled(*editor, retrochip::ids::gain, true);
    checkEnabled(*editor, retrochip::ids::velocity, true);
    checkLayout(*editor);

    auto* label = dynamic_cast<juce::Label*>(findById(*editor, "genStatus"));
    CHECK(label != nullptr && label->getText().contains("WAV"));
    processor.importGenesisKit(files);
    CHECK(label != nullptr && label->getText().contains("3"));
    CHECK(label != nullptr && label->getTooltip().contains("hat"));
    CHECK(label != nullptr && label->getTooltip().contains("snare"));
    saveSnapshot(*editor, "editor_ym2612_kit.png");

    // Picked in the menu, as a user does, the row beside it changes as well.
    auto* box = dynamic_cast<juce::ComboBox*>(findById(*editor, retrochip::ids::genPatch));
    CHECK(box != nullptr);
    if (box == nullptr)
        return;
    box->setSelectedItemIndex(2, juce::sendNotificationSync);
    CHECK(visible(retrochip::ids::genAlgorithm));
    checkEnabled(*editor, retrochip::ids::genRelease, true);
    box->setSelectedItemIndex(GenesisEngine::kPcmKitPatch, juce::sendNotificationSync);
    CHECK(!visible(retrochip::ids::genAlgorithm));
    checkEnabled(*editor, retrochip::ids::genRelease, false);

    // The single sample has them back, and leaving the kit gives the 2A03 its own rules.
    setParam(processor, retrochip::ids::genPatch, static_cast<float>(GenesisEngine::kPcmPatch));
    for (const char* id : { retrochip::ids::genRootNote, retrochip::ids::genLoop, retrochip::ids::genRelease, retrochip::ids::vibratoDepth, retrochip::ids::glide })
        checkEnabled(*editor, id, true);

    setParam(processor, retrochip::ids::genPatch, static_cast<float>(GenesisEngine::kPcmKitPatch));
    setParam(processor, kConsoleId, 0.0f);
    checkEnabled(*editor, retrochip::ids::vibratoDepth, true);
    checkEnabled(*editor, retrochip::ids::glide, true);
}

constexpr int kPsgConsole = 4;

void psg_is_a_console_with_its_engine_and_latency()
{
    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(processor.engineFor(Console::Psg) != nullptr);
    auto* psg = dynamic_cast<retrochip::PsgEngine*>(processor.engineFor(Console::Psg));
    CHECK(psg != nullptr);
    if (psg == nullptr)
        return;

    CHECK(setConsole(processor, kPsgConsole));
    CHECK(processor.getActiveConsole() == Console::Psg);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    CHECK(psg->latencySamples() > 0);
    CHECK(processor.getLatencySamples() == psg->latencySamples());

    processor.prepareToPlay(kSampleRate, kBlock);
    const auto played = playUserNote(processor, 69, 40);
    CHECK(peakOf(played) > 0.05);
    CHECK(std::abs(pcmFrequency(played, kSampleRate, 2 * kBlock) - 440.4) < 3.0);

    // A note held when the console changes stops, in both directions.
    CHECK(psg->activeVoices() == 1);
    setConsole(processor, 0);
    run(processor, 2);
    CHECK(psg->activeVoices() == 0);
}

void psg_parameters_reach_the_engine()
{
    RetroChipProcessor processor;
    setConsole(processor, kPsgConsole);
    const auto render = [&](const char* id, float value)
    {
        const auto before = getParam(processor, id);
        setParam(processor, id, value);
        auto played = playUserNote(processor, 57, 30);
        setParam(processor, id, before);
        return played;
    };
    const auto base = playUserNote(processor, 57, 30);
    const auto differs = [&](const std::vector<float>& other)
    {
        double most = 0.0;
        for (size_t i = 0; i < base.size(); ++i)
            most = std::max(most, static_cast<double>(std::abs(base[i] - other[i])));
        return most > 0.01;
    };

    CHECK(!differs(playUserNote(processor, 57, 30))); // the same settings play the same
    CHECK(differs(render(retrochip::ids::psgAttack, 200.0f)));
    CHECK(differs(render(retrochip::ids::psgDecay, 200.0f)) == false); // nothing to decay to at full sustain
    CHECK(differs(render(retrochip::ids::psgSustain, 4.0f)));
    CHECK(differs(render(retrochip::ids::psgChannel, 1.0f)));

    setParam(processor, retrochip::ids::psgChannel, 1.0f);
    const auto white = playUserNote(processor, 57, 30);
    setParam(processor, retrochip::ids::psgNoiseType, 1.0f);
    const auto periodic = playUserNote(processor, 57, 30);
    setParam(processor, retrochip::ids::psgNoiseType, 0.0f);
    setParam(processor, retrochip::ids::psgNoiseRate, 0.0f);
    const auto fixedRate = playUserNote(processor, 57, 30);
    double typeDifference = 0.0, rateDifference = 0.0;
    for (size_t i = 0; i < white.size(); ++i)
    {
        typeDifference = std::max(typeDifference, static_cast<double>(std::abs(white[i] - periodic[i])));
        rateDifference = std::max(rateDifference, static_cast<double>(std::abs(white[i] - fixedRate[i])));
    }
    CHECK(typeDifference > 0.01);
    CHECK(rateDifference > 0.01);
}

void psg_presets_set_their_controls()
{
    RetroChipProcessor processor;
    juce::Array<float> seenChannels;
    for (int i = 36; i < 48; ++i)
    {
        const auto& preset = retrochip::kPresets[static_cast<size_t>(i)];
        processor.setCurrentProgram(i);
        CHECK(processor.getProgramName(i).startsWith("PSG"));
        CHECK(getParam(processor, retrochip::ids::psgChannel) == static_cast<float>(preset.psgChannel));
        CHECK(getParam(processor, retrochip::ids::psgNoiseType) == static_cast<float>(preset.psgNoiseType));
        CHECK(getParam(processor, retrochip::ids::psgNoiseRate) == static_cast<float>(preset.psgNoiseRate));
        CHECK(getParam(processor, retrochip::ids::psgAttack) == preset.psgAttack);
        CHECK(getParam(processor, retrochip::ids::psgDecay) == preset.psgDecay);
        CHECK(getParam(processor, retrochip::ids::psgSustain) == preset.psgSustain);
        CHECK(getParam(processor, retrochip::ids::psgRelease) == preset.psgRelease);
        CHECK(getParam(processor, retrochip::ids::vibratoDepth) == preset.vibratoDepth);
    }

    // A preset of the chip leaves the others' controls alone, and the other way round.
    setParam(processor, retrochip::ids::attack, 333.0f);
    processor.setCurrentProgram(40);
    CHECK(getParam(processor, retrochip::ids::attack) == 333.0f);
    setParam(processor, retrochip::ids::psgAttack, 444.0f);
    processor.setCurrentProgram(3);
    CHECK(getParam(processor, retrochip::ids::psgAttack) == 444.0f);
}

void psg_settings_round_trip_in_the_state()
{
    juce::MemoryBlock saved;
    {
        RetroChipProcessor processor;
        processor.setCurrentProgram(42); // PSG Bass
        setParam(processor, retrochip::ids::psgRelease, 123.0f);
        processor.getStateInformation(saved);
    }
    RetroChipProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    CHECK(restored.getActiveConsole() == Console::Psg);
    CHECK(restored.getCurrentProgram() == 42);
    CHECK(getParam(restored, retrochip::ids::psgChannel) == 1.0f);
    CHECK(getParam(restored, retrochip::ids::psgNoiseType) == 1.0f);
    CHECK(getParam(restored, retrochip::ids::psgRelease) == 123.0f);
    CHECK(peakOf(playUserNote(restored, 36, 20)) > 0.02);
}

void psg_panel_shows_only_its_controls()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    const auto visible = [&](const char* id)
    {
        auto* component = findById(*editor, id);
        CHECK(component != nullptr);
        return component != nullptr && component->isVisible();
    };
    const char* const own[] = { retrochip::ids::psgChannel, retrochip::ids::psgNoiseType, retrochip::ids::psgNoiseRate, retrochip::ids::psgAttack,
                                retrochip::ids::psgDecay, retrochip::ids::psgSustain, retrochip::ids::psgRelease };

    for (int console : { 0, 1, 2, 3 })
    {
        setParam(processor, kConsoleId, static_cast<float>(console));
        for (const char* id : own)
            CHECK(!visible(id));
    }

    auto* button = dynamic_cast<juce::Button*>(findById(*editor, "console_4"));
    CHECK(button != nullptr);
    if (button == nullptr)
        return;
    button->setToggleState(true, juce::sendNotificationSync);
    button->onClick();
    CHECK(getParam(processor, kConsoleId) == static_cast<float>(kPsgConsole));

    for (const char* id : own)
        CHECK(visible(id));
    for (const char* id : { retrochip::ids::vibratoRate, retrochip::ids::vibratoDepth, retrochip::ids::glide, retrochip::ids::velocity, retrochip::ids::gain })
        CHECK(visible(id));
    for (const char* id : { retrochip::ids::channel, retrochip::ids::genPatch, retrochip::ids::genAlgorithm, retrochip::ids::snesInstrument, "sfFile" })
        CHECK(!visible(id));
    checkTitles(*editor);
    checkLayout(*editor);

    // A tone has no noise settings.
    checkEnabled(*editor, retrochip::ids::psgNoiseType, false);
    checkEnabled(*editor, retrochip::ids::psgNoiseRate, false);
    checkEnabled(*editor, retrochip::ids::vibratoDepth, true);
    saveSnapshot(*editor, "editor_sn76489.png");

    // Noise has them, and at a fixed rate it has no pitch to bend or slide.
    setParam(processor, retrochip::ids::psgChannel, 1.0f);
    checkEnabled(*editor, retrochip::ids::psgNoiseType, true);
    checkEnabled(*editor, retrochip::ids::psgNoiseRate, true);
    checkEnabled(*editor, retrochip::ids::vibratoDepth, true);
    setParam(processor, retrochip::ids::psgNoiseRate, 1.0f);
    for (const char* id : { retrochip::ids::vibratoRate, retrochip::ids::vibratoDepth, retrochip::ids::glide })
        checkEnabled(*editor, id, false);
    checkEnabled(*editor, retrochip::ids::gain, true);
    checkLayout(*editor);
    saveSnapshot(*editor, "editor_sn76489_noise.png");

    // None of that follows the other consoles.
    setParam(processor, kConsoleId, 1.0f);
    checkEnabled(*editor, retrochip::ids::vibratoDepth, true);
    setParam(processor, kConsoleId, 0.0f);
    checkEnabled(*editor, retrochip::ids::vibratoDepth, true);
}

void preset_menu_lists_the_psg_after_the_others()
{
    RetroChipProcessor processor;
    setParam(processor, kConsoleId, static_cast<float>(kPsgConsole));
    processor.setCurrentProgram(38);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto* preset = dynamic_cast<juce::ComboBox*>(findById(*editor, "preset"));
    CHECK(preset != nullptr);
    if (preset != nullptr)
        CHECK(preset->getText() == "PSG Arp");
    checkLayout(*editor);
}

// The value box under a knob, with the outline it is drawn with.
juce::Colour valueBoxOutline(juce::Component& editor, const char* knobId)
{
    auto* knob = findById(editor, knobId);
    for (auto* child : knob != nullptr ? knob->getChildren() : juce::Array<juce::Component*>())
        if (auto* slider = dynamic_cast<juce::Slider*>(child))
            for (auto* part : slider->getChildren())
                if (auto* box = dynamic_cast<juce::Label*>(part))
                    return box->findColour(juce::Label::outlineColourId);
    CHECK(false);
    return {};
}

void value_boxes_keep_their_outline_across_consoles()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    const auto expected = retrochip::palette::valueBoxEdge;
    CHECK(valueBoxOutline(*editor, retrochip::ids::vibratoRate) == expected);
    CHECK(valueBoxOutline(*editor, retrochip::ids::gain) == expected);

    // The SNES's knobs are compact, which builds their boxes again; leaving it must not lose the outline.
    setParam(processor, kConsoleId, 2.0f);
    CHECK(valueBoxOutline(*editor, retrochip::ids::vibratoRate) == expected);
    CHECK(valueBoxOutline(*editor, retrochip::ids::snesAttack) == expected);
    for (float console : { 0.0f, 1.0f, 4.0f })
    {
        setParam(processor, kConsoleId, console);
        CHECK(valueBoxOutline(*editor, retrochip::ids::vibratoRate) == expected);
        CHECK(valueBoxOutline(*editor, retrochip::ids::gain) == expected);
    }
}

constexpr int kVrc6Console = 5;
constexpr int kN163Console = 6;
using retrochip::NesImport;

// How strong one frequency is in a signal: a way to read the pitch of the delta-modulation channel's noisy output.
double toneMagnitude(const std::vector<float>& x, double frequency, size_t from = 0)
{
    double re = 0.0, im = 0.0;
    for (size_t i = from; i < x.size(); ++i)
    {
        const double phase = 2.0 * 3.14159265358979323846 * frequency * static_cast<double>(i) / kSampleRate;
        re += x[i] * std::cos(phase);
        im += x[i] * std::sin(phase);
    }
    return std::sqrt(re * re + im * im) / static_cast<double>(x.size() - from);
}

// An .fti file as FamiTracker writes it, version 2.4.
struct FtiFile
{
    std::vector<juce::uint8> bytes;

    FtiFile(int chip, const std::string& name)
    {
        for (const char c : std::string("FTI2.4"))
            bytes.push_back(static_cast<juce::uint8>(c));
        int8(chip);
        text(name);
    }
    void int8(int v) { bytes.push_back(static_cast<juce::uint8>(v)); }
    void int32(int v)
    {
        for (int i = 0; i < 4; ++i)
            bytes.push_back(static_cast<juce::uint8>((static_cast<unsigned>(v) >> (8 * i)) & 0xFF));
    }
    void text(const std::string& t)
    {
        int32(static_cast<int>(t.size()));
        bytes.insert(bytes.end(), t.begin(), t.end());
    }
    void sequences(const std::vector<int>& volume, int release = -1, const std::vector<int>& duty = {})
    {
        int8(5);
        const std::vector<int>* slots[] = { &volume, nullptr, nullptr, nullptr, &duty };
        for (int i = 0; i < 5; ++i)
        {
            const auto* values = slots[i];
            const bool on = values != nullptr && !values->empty();
            int8(on ? 1 : 0);
            if (!on)
                continue;
            int32(static_cast<int>(values->size()));
            int32(-1);
            int32(i == 0 ? release : -1);
            int32(0);
            for (const int v : *values)
                int8(v);
        }
    }
    void save(const juce::File& file) const { file.replaceWithData(bytes.data(), bytes.size()); }
};

// A 2A03 instrument. With `kit`, it has a sample on MIDI note 48.
void writeFti2a03(const juce::File& file, const std::string& name, const std::vector<int>& volume, bool kit = false)
{
    FtiFile fti(1, name);
    fti.sequences(volume);
    if (kit)
    {
        fti.int32(1);
        fti.int8(36); fti.int8(1); fti.int8(15); fti.int8(-1);
        fti.int32(1);
        std::vector<float> sine(20000);
        for (size_t i = 0; i < sine.size(); ++i)
            sine[i] = 0.8f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 440.0 * static_cast<double>(i) / retrochip::dpcm::rateHz(15)));
        const auto encoded = retrochip::dpcm::encode(sine);
        fti.int32(0);
        fti.text("kick");
        fti.text(std::string(encoded.begin(), encoded.end()));
    }
    else
    {
        fti.int32(0);
        fti.int32(0);
    }
    fti.save(file);
}

void writeFtiVrc6(const juce::File& file, const std::string& name)
{
    FtiFile fti(2, name);
    fti.sequences({ 15, 12, 9 }, -1, { 1 });
    fti.save(file);
}

void writeFtiN163(const juce::File& file, const std::string& name)
{
    FtiFile fti(5, name);
    fti.sequences({ 15 });
    fti.int32(32); fti.int32(0); fti.int32(1);
    for (const auto v : retrochip::n163Waves()[3])
        fti.int8(v);
    fti.save(file);
}

void useNes(RetroChipProcessor& processor, int console)
{
    setParam(processor, kConsoleId, static_cast<float>(console));
    setParam(processor, retrochip::ids::vibratoDepth, 0.0f);
    setParam(processor, retrochip::ids::glide, 0.0f);
    setParam(processor, retrochip::ids::attack, 0.0f);
    setParam(processor, retrochip::ids::decay, 0.0f);
    setParam(processor, retrochip::ids::sustain, 15.0f);
}

void nes_chips_are_consoles_on_the_nes_engine()
{
    RetroChipProcessor processor;
    auto* nes = processor.engineFor(Console::Chip2A03);
    CHECK(processor.engineFor(Console::Vrc6) == nes);
    CHECK(processor.engineFor(Console::N163) == nes);
    auto* channel = dynamic_cast<juce::AudioParameterChoice*>(processor.apvts.getParameter(retrochip::ids::channel));
    CHECK(channel != nullptr && channel->choices == juce::StringArray({ "Pulse", "Triangle", "Noise", "DPCM" }));

    for (int console : { kVrc6Console, kN163Console })
    {
        useNes(processor, console);
        CHECK(processor.getActiveConsole() == (console == kVrc6Console ? Console::Vrc6 : Console::N163));
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        CHECK(processor.getLatencySamples() == retrochip::BandLimitedBuffer::kLatencySamples);
        const auto played = playUserNote(processor, 69, 20);
        CHECK(peakOf(played) > 0.05);
        CHECK(std::abs(pcmFrequency(played, kSampleRate, 2 * kBlock) - 440.0) < 5.0); // the N163's steps would fool a plain count
    }
}

void cartridge_chip_parameters_reach_the_engine()
{
    RetroChipProcessor processor;
    const auto differs = [&](const char* id, float value)
    {
        const auto base = playUserNote(processor, 57, 20);
        const auto before = getParam(processor, id);
        setParam(processor, id, value);
        const auto other = playUserNote(processor, 57, 20);
        setParam(processor, id, before);
        double most = 0.0;
        for (size_t i = 0; i < base.size(); ++i)
            most = std::max(most, static_cast<double>(std::abs(base[i] - other[i])));
        return most > 0.01;
    };
    useNes(processor, kVrc6Console);
    CHECK(differs(retrochip::ids::vrc6Channel, 1.0f));
    CHECK(differs(retrochip::ids::vrc6Duty, 2.0f));
    useNes(processor, kN163Console);
    CHECK(differs(retrochip::ids::n163Wave, 3.0f));
    CHECK(differs(retrochip::ids::n163Channels, 8.0f));
}

void dpcm_wav_import_plays_on_its_root_note()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("tone.wav");
    writeMono(file, sineWave(440.0, 32000.0, 0.5, 0.8f));

    RetroChipProcessor processor;
    useNes(processor, 0);
    setParam(processor, retrochip::ids::channel, 3.0f);
    setParam(processor, retrochip::ids::dpcmRootNote, 69.0f);
    processor.prepareToPlay(kSampleRate, kBlock);
    CHECK(run(processor, 4, 69) == 0.0); // nothing imported yet
    allOff(processor);
    CHECK(processor.getNesStatus().contains("WAV"));

    auto kind = NesImport::none;
    CHECK(processor.importNesFile(file, &kind).isEmpty());
    CHECK(kind == NesImport::dpcm);
    CHECK(processor.getDpcmName() == "tone");
    CHECK(processor.getNesStatus() == "tone");

    const auto played = playUserNote(processor, 69, 30);
    CHECK(peakOf(played) > 0.05);
    CHECK(toneMagnitude(played, 440.0, 2 * kBlock) > 3.0 * toneMagnitude(played, 470.0, 2 * kBlock));

    // Longer than the chip can hold, the sample is cut, and says so.
    const auto longFile = folder.dir.getChildFile("long.wav");
    writeMono(longFile, sineWave(220.0, 32000.0, 2.0));
    CHECK(processor.importNesFile(longFile, &kind).contains("4081"));
    CHECK(kind == NesImport::dpcm);
}

void fti_import_brings_the_instrument_to_its_chip()
{
    TempFolder folder;
    const auto lead = folder.dir.getChildFile("lead.fti");
    std::vector<int> fading(10, 15);
    fading.push_back(0);
    writeFti2a03(lead, "Lead", fading);
    const auto saw = folder.dir.getChildFile("saw.fti");
    writeFtiVrc6(saw, "Saw");
    const auto waves = folder.dir.getChildFile("waves.fti");
    writeFtiN163(waves, "Waves");

    RetroChipProcessor processor;
    useNes(processor, 0);
    auto kind = NesImport::none;
    int chip = -1;
    CHECK(processor.importNesFile(lead, &kind, &chip).isEmpty());
    CHECK(kind == NesImport::instrument && chip == 0);
    CHECK(processor.getNesInstrumentName(0) == "Lead");
    CHECK(processor.importNesFile(saw, &kind, &chip).isEmpty());
    CHECK(chip == 1 && processor.getNesInstrumentName(1) == "Saw");
    CHECK(processor.importNesFile(waves, &kind, &chip).isEmpty());
    CHECK(chip == 2 && processor.getNesInstrumentName(2) == "Waves");
    CHECK(processor.getNesInstrumentName(0) == "Lead"); // each chip has its own

    // With the instrument on, the volume sequence ends the note by itself.
    setParam(processor, retrochip::ids::nesUseInstrument, 1.0f);
    CHECK(processor.getNesStatus() == "Lead");
    const auto withInstrument = playUserNote(processor, 69, 40);
    CHECK(peakOf(withInstrument, 0, 4 * kBlock) > 0.05);
    CHECK(peakOf(withInstrument, withInstrument.size() - 4 * kBlock) < 1e-3);
    setParam(processor, retrochip::ids::nesUseInstrument, 0.0f);
    const auto withEnvelope = playUserNote(processor, 69, 40);
    CHECK(peakOf(withEnvelope, withEnvelope.size() - 4 * kBlock) > 0.05);
}

void fti_import_refuses_what_it_cannot_play()
{
    TempFolder folder;
    const auto good = folder.dir.getChildFile("good.fti");
    writeFti2a03(good, "Good", { 15 });
    const auto vrc7 = folder.dir.getChildFile("vrc7.fti");
    FtiFile(3, "FM").save(vrc7);
    const auto cut = folder.dir.getChildFile("cut.fti");
    FtiFile damaged(1, "Cut");
    damaged.int8(5);
    damaged.save(cut);
    const auto text = folder.dir.getChildFile("text.fti");
    text.replaceWithText("hello");
    const auto other = folder.dir.getChildFile("other.mid");
    other.replaceWithText("x");

    RetroChipProcessor processor;
    processor.importNesFile(good);
    for (const auto& file : { vrc7, cut, text, other, folder.dir.getChildFile("missing.fti") })
    {
        auto kind = NesImport::instrument;
        const auto message = processor.importNesFile(file, &kind);
        CHECK(kind == NesImport::none);
        CHECK(message.contains(file.getFileName()));
        CHECK(processor.getNesInstrumentName(0) == "Good");
    }
    CHECK(processor.importNesFile(vrc7).contains("VRC7"));
}

void fti_kit_plays_its_samples_on_the_dpcm_channel()
{
    TempFolder folder;
    const auto file = folder.dir.getChildFile("drums.fti");
    writeFti2a03(file, "Drums", {}, true);

    RetroChipProcessor processor;
    useNes(processor, 0);
    setParam(processor, retrochip::ids::channel, 3.0f);
    setParam(processor, retrochip::ids::nesUseInstrument, 1.0f);
    CHECK(processor.importNesFile(file).isEmpty());
    CHECK(processor.getNesKitDescription().contains("kick"));
    CHECK(processor.getNesStatus().contains("1"));
    CHECK(peakOf(playUserNote(processor, 48, 20)) > 0.05);
    CHECK(peakOf(playUserNote(processor, 49, 8)) == 0.0);
}

void nes_imports_round_trip_in_the_state()
{
    TempFolder folder;
    const auto wav = folder.dir.getChildFile("tone.wav");
    writeMono(wav, sineWave(440.0, 32000.0, 0.5, 0.8f));
    const auto lead = folder.dir.getChildFile("lead.fti");
    writeFti2a03(lead, "Lead", { 15 });
    const auto saw = folder.dir.getChildFile("saw.fti");
    writeFtiVrc6(saw, "Saw");
    const auto waves = folder.dir.getChildFile("waves.fti");
    writeFtiN163(waves, "Waves");

    juce::MemoryBlock saved;
    {
        RetroChipProcessor processor;
        useNes(processor, 0);
        setParam(processor, retrochip::ids::channel, 3.0f);
        setParam(processor, retrochip::ids::dpcmRootNote, 69.0f);
        for (const auto& file : { wav, lead, saw, waves })
            processor.importNesFile(file);
        processor.getStateInformation(saved);
    }
    folder.dir.deleteRecursively();

    RetroChipProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    CHECK(restored.getDpcmName() == "tone");
    CHECK(restored.getNesInstrumentName(0) == "Lead");
    CHECK(restored.getNesInstrumentName(1) == "Saw");
    CHECK(restored.getNesInstrumentName(2) == "Waves");
    const auto played = playUserNote(restored, 69, 30);
    CHECK(peakOf(played) > 0.05);
    for (const auto* property : { retrochip::kNesDpcmProperty, retrochip::kNesFtiProperties[0], retrochip::kNesFtiProperties[2] })
        CHECK(!restored.apvts.state.hasProperty(property));

    RetroChipProcessor blank;
    juce::MemoryBlock nothing;
    blank.getStateInformation(nothing);
    restored.setStateInformation(nothing.getData(), static_cast<int>(nothing.getSize()));
    CHECK(restored.getDpcmName().isEmpty());
    CHECK(restored.getNesInstrumentName(1).isEmpty());

    // What cannot be read is left out, with a message.
    auto xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
    CHECK(xml != nullptr);
    if (xml == nullptr)
        return;
    xml->setAttribute(retrochip::kNesFtiProperties[1], "AAAA");
    xml->setAttribute(retrochip::kNesDpcmProperty, "!!");
    juce::MemoryBlock damaged;
    juce::AudioProcessor::copyXmlToBinary(*xml, damaged);
    RetroChipProcessor partly;
    partly.setStateInformation(damaged.getData(), static_cast<int>(damaged.getSize()));
    CHECK(partly.getNesInstrumentName(0) == "Lead");
    CHECK(partly.getNesInstrumentName(1).isEmpty());
    CHECK(partly.getDpcmName().isEmpty());
    CHECK(partly.getNesStatus().containsIgnoreCase("unreadable"));
}

void replaced_nes_sounds_wait_for_the_audio_thread()
{
    TempFolder folder;
    const auto lead = folder.dir.getChildFile("lead.fti");
    writeFti2a03(lead, "Lead", { 15 });

    RetroChipProcessor processor;
    processor.prepareToPlay(kSampleRate, kBlock);
    for (int i = 0; i < 4; ++i)
        processor.importNesFile(lead);
    CHECK(processor.getRetiredNesSoundCount() == 0);
    processWith(processor, 1);
    processor.importNesFile(lead);
    CHECK(processor.getRetiredNesSoundCount() == 1);
    processWith(processor, 1);
    processor.releaseRetiredSamples();
    CHECK(processor.getRetiredNesSoundCount() == 0);
}

void cartridge_presets_set_their_chip()
{
    RetroChipProcessor processor;
    setParam(processor, retrochip::ids::nesUseInstrument, 1.0f);
    for (int i = 48; i < 72; ++i)
    {
        const auto& preset = retrochip::kPresets[static_cast<size_t>(i)];
        processor.setCurrentProgram(i);
        CHECK(preset.console == (i < 60 ? kVrc6Console : kN163Console));
        CHECK(getParam(processor, retrochip::ids::nesUseInstrument) == 0.0f);
        CHECK(getParam(processor, retrochip::ids::attack) == preset.attack);
        CHECK(getParam(processor, retrochip::ids::release) == preset.release);
        if (i < 60)
        {
            CHECK(getParam(processor, retrochip::ids::vrc6Channel) == static_cast<float>(preset.vrc6Channel));
            CHECK(getParam(processor, retrochip::ids::vrc6Duty) == static_cast<float>(preset.vrc6Duty));
        }
        else
        {
            CHECK(getParam(processor, retrochip::ids::n163Wave) == static_cast<float>(preset.n163Wave));
            CHECK(getParam(processor, retrochip::ids::n163Channels) == static_cast<float>(preset.n163Channels));
        }
    }
}

void nes_panels_follow_the_chip_and_channel()
{
    TempFolder folder;
    const auto saw = folder.dir.getChildFile("saw.fti");
    writeFtiVrc6(saw, "Saw");

    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    const auto visible = [&](const char* id)
    {
        auto* component = findById(*editor, id);
        CHECK(component != nullptr);
        return component != nullptr && component->isVisible();
    };
    const char* const shared[] = { retrochip::ids::attack, retrochip::ids::decay, retrochip::ids::sustain, retrochip::ids::release,
                                   "nesImport", "nesStatus", retrochip::ids::nesUseInstrument };
    const char* const own2a03[] = { retrochip::ids::channel, retrochip::ids::duty, retrochip::ids::noiseMode };
    const char* const ownVrc6[] = { retrochip::ids::vrc6Channel, retrochip::ids::vrc6Duty };
    const char* const ownN163[] = { retrochip::ids::n163Wave, retrochip::ids::n163Channels };

    for (int console : { 0, kVrc6Console, kN163Console })
    {
        setParam(processor, kConsoleId, static_cast<float>(console));
        for (const char* id : shared)
            CHECK(visible(id));
        for (const char* id : own2a03)
            CHECK(visible(id) == (console == 0));
        for (const char* id : ownVrc6)
            CHECK(visible(id) == (console == kVrc6Console));
        for (const char* id : ownN163)
            CHECK(visible(id) == (console == kN163Console));
        CHECK(!visible(retrochip::ids::dpcmRootNote));
        checkTitles(*editor);
        checkLayout(*editor);
        if (console == kVrc6Console)
            saveSnapshot(*editor, "editor_vrc6.png");
        if (console == kN163Console)
            saveSnapshot(*editor, "editor_n163.png");
    }
    for (int console : { 1, 2, 3, 4 })
    {
        setParam(processor, kConsoleId, static_cast<float>(console));
        for (const char* id : shared)
            CHECK(!visible(id));
    }

    // The delta-modulation channel has a root note and a loop instead of the duty and the noise mode, and no volume.
    setParam(processor, kConsoleId, 0.0f);
    setParam(processor, retrochip::ids::channel, 3.0f);
    CHECK(visible(retrochip::ids::dpcmRootNote) && visible(retrochip::ids::dpcmLoop));
    CHECK(!visible(retrochip::ids::duty) && !visible(retrochip::ids::noiseMode));
    for (const char* id : { retrochip::ids::attack, retrochip::ids::sustain, retrochip::ids::vibratoDepth, retrochip::ids::glide, retrochip::ids::velocity })
        checkEnabled(*editor, id, false);
    checkLayout(*editor);
    saveSnapshot(*editor, "editor_dpcm.png");
    setParam(processor, retrochip::ids::channel, 0.0f);
    CHECK(visible(retrochip::ids::duty) && !visible(retrochip::ids::dpcmRootNote));
    checkEnabled(*editor, retrochip::ids::attack, true);

    // The instrument switch waits for an instrument, and takes the envelope's place.
    setParam(processor, kConsoleId, static_cast<float>(kVrc6Console));
    checkEnabled(*editor, retrochip::ids::nesUseInstrument, false);
    auto* concrete = dynamic_cast<retrochip::RetroChipEditor*>(editor.get());
    CHECK(concrete != nullptr);
    if (concrete == nullptr)
        return;
    concrete->importNesFile(saw);
    CHECK(getParam(processor, retrochip::ids::nesUseInstrument) == 1.0f);
    checkEnabled(*editor, retrochip::ids::nesUseInstrument, true);
    for (const char* id : { retrochip::ids::attack, retrochip::ids::decay, retrochip::ids::sustain, retrochip::ids::release })
        checkEnabled(*editor, id, false);
    auto* label = dynamic_cast<juce::Label*>(findById(*editor, "nesStatus"));
    CHECK(label != nullptr && label->getText() == "Saw");
    setParam(processor, retrochip::ids::nesUseInstrument, 0.0f);
    checkEnabled(*editor, retrochip::ids::attack, true);

    // The 2A03 has no instrument yet, so its switch is off limits.
    setParam(processor, kConsoleId, 0.0f);
    checkEnabled(*editor, retrochip::ids::nesUseInstrument, false);
}

void editor_nes_import_picks_the_chip_and_channel()
{
    TempFolder folder;
    const auto wav = folder.dir.getChildFile("tone.wav");
    writeMono(wav, sineWave(440.0, 32000.0, 0.3, 0.8f));
    const auto saw = folder.dir.getChildFile("saw.fti");
    writeFtiVrc6(saw, "Saw");
    const auto waves = folder.dir.getChildFile("waves.fti");
    writeFtiN163(waves, "Waves");
    const auto drums = folder.dir.getChildFile("drums.fti");
    writeFti2a03(drums, "Drums", {}, true);
    const auto lead = folder.dir.getChildFile("lead.fti");
    writeFti2a03(lead, "Lead", { 15, 10 });

    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto* concrete = dynamic_cast<retrochip::RetroChipEditor*>(editor.get());
    CHECK(concrete != nullptr);
    if (concrete == nullptr)
        return;
    const auto console = [&] { return juce::roundToInt(getParam(processor, kConsoleId)); };
    const auto channel = [&] { return juce::roundToInt(getParam(processor, retrochip::ids::channel)); };
    const auto instrumentOn = [&] { return getParam(processor, retrochip::ids::nesUseInstrument) == 1.0f; };

    concrete->importNesFile(saw);
    CHECK(console() == kVrc6Console && instrumentOn());
    concrete->importNesFile(waves);
    CHECK(console() == kN163Console && instrumentOn());
    concrete->importNesFile(wav);
    CHECK(console() == 0 && channel() == 3 && !instrumentOn());
    concrete->importNesFile(drums); // samples and no volume sequence: a kit
    CHECK(console() == 0 && channel() == 3 && instrumentOn());
    setParam(processor, retrochip::ids::channel, 1.0f);
    concrete->importNesFile(lead); // a melodic instrument keeps the channel
    CHECK(console() == 0 && channel() == 1 && instrumentOn());
    concrete->importNesFile(folder.dir.getChildFile("missing.fti"));
    CHECK(console() == 0 && channel() == 1);
}

// The folders of .tfi timbres and .fti instruments, and the two languages.

juce::Button* buttonById(juce::Component& editor, const char* id)
{
    auto* button = dynamic_cast<juce::Button*>(findById(editor, id));
    CHECK(button != nullptr);
    return button;
}

void click(juce::Component& editor, const char* id)
{
    if (auto* button = buttonById(editor, id))
        button->onClick(); // triggerClick() is asynchronous
}

bool isShown(juce::Component& editor, const char* id)
{
    auto* component = findById(editor, id);
    CHECK(component != nullptr);
    return component != nullptr && component->isVisible();
}

float paramValue(RetroChipProcessor& processor, const char* id)
{
    return processor.apvts.getRawParameterValue(id)->load();
}

// Writes a file and the folders it needs.
juce::File inFolder(const juce::File& root, const char* path)
{
    const auto file = root.getChildFile(path);
    CHECK(file.getParentDirectory().createDirectory().wasOk());
    return file;
}

// The tests run in English; a scope in French goes back to it.
struct InFrench
{
    InFrench() { retrochip::setLanguage(retrochip::Language::french); }
    ~InFrench() { retrochip::setLanguage(retrochip::Language::english); }
};

void library_folders_are_under_the_music_folder()
{
    const auto music = juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    CHECK(retrochip::defaultLibraryRoot() == music.getChildFile("RetroChip/SoundFonts"));
    CHECK(retrochip::defaultLibraryRoot(retrochip::Library::tfi) == music.getChildFile("RetroChip/Genesis"));
    CHECK(retrochip::defaultLibraryRoot(retrochip::Library::fti) == music.getChildFile("RetroChip/NES"));

    RetroChipProcessor processor;
    CHECK(processor.getLibraryRoot(retrochip::Library::tfi) == music.getChildFile("RetroChip/Genesis"));
    CHECK(processor.getLibraryRoot(retrochip::Library::fti) == music.getChildFile("RetroChip/NES"));
    TempFolder folder;
    processor.setLibraryRoot(retrochip::Library::fti, folder.dir);
    CHECK(processor.getLibraryRoot(retrochip::Library::fti) == folder.dir);
    CHECK(processor.getLibraryRoot(retrochip::Library::tfi) == music.getChildFile("RetroChip/Genesis"));
    CHECK(processor.getLibraryRoot() == music.getChildFile("RetroChip/SoundFonts"));
}

void scan_keeps_the_library_extension()
{
    TempFolder folder;
    for (const char* path : { "a.tfi", "b.TFI", "c.fti", "d.sf2", "Sega/e.tfi", "notes.txt" })
        CHECK(inFolder(folder.dir, path).create().wasOk());

    juce::String message;
    auto names = [](const std::vector<retrochip::LibraryEntry>& entries)
    {
        juce::StringArray result;
        for (const auto& entry : entries)
            result.add(entry.relativePath);
        return result;
    };
    CHECK(names(retrochip::scanLibrary(folder.dir, message, retrochip::Library::tfi)) == juce::StringArray({ "Sega/e.tfi", "a.tfi", "b.TFI" }));
    CHECK(message.isEmpty());
    CHECK(names(retrochip::scanLibrary(folder.dir, message, retrochip::Library::fti)) == juce::StringArray({ "c.fti" }));
    CHECK(names(retrochip::scanLibrary(folder.dir, message)) == juce::StringArray({ "d.sf2" }));

    const auto empty = folder.dir.getChildFile("none");
    CHECK(retrochip::scanLibrary(empty, message, retrochip::Library::tfi).empty());
    CHECK(message == "No timbre: add .tfi files to the folder");
    CHECK(retrochip::scanLibrary(empty, message, retrochip::Library::fti).empty());
    CHECK(message == "No instrument: add .fti files to the folder");
}

void tfi_folder_lists_its_timbres_and_imports_one()
{
    TempFolder folder;
    writeTfi(inFolder(folder.dir, "Sonic/Lead.tfi"), 4, 5);
    writeTfi(inFolder(folder.dir, "Bass.tfi"));
    CHECK(folder.dir.getChildFile("notes.txt").replaceWithText("not a timbre"));
    writeFtiVrc6(folder.dir.getChildFile("Sonic/saw.fti"), "Saw");

    RetroChipProcessor processor;
    processor.setLibraryRoot(retrochip::Library::tfi, folder.dir);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    CHECK(!isShown(*editor, "genLibraryPanel"));
    setConsole(processor, 1);
    CHECK(isShown(*editor, "genLibraryPanel"));
    CHECK(!isShown(*editor, "nesLibraryPanel"));

    auto* box = comboById(*editor, "genLibrary");
    if (box == nullptr)
        return;
    CHECK((menuRows(*box) == Rows { "# Sonic", "Lead", "# Other", "Bass" }));
    CHECK(box->getTextWhenNothingSelected() == "From the folder");
    CHECK(box->getTooltip() == "Folder: " + folder.dir.getFullPathName());
    CHECK(buttonById(*editor, "genOpenFolder")->getTooltip() == "Opens the folder of .tfi timbres, and creates it if it does not exist");
    CHECK(buttonById(*editor, "genRefresh")->getTooltip() == "Reads the folder again to find the files added");
    CHECK(buttonById(*editor, "genRefresh")->getTitle().isNotEmpty());

    // A pick is an import: the timbre is the user's, with its algorithm and feedback, and the menu waits again.
    if (choose(*box, "Lead"))
    {
        CHECK(processor.getUserPatchName() == "Lead");
        CHECK(juce::roundToInt(paramValue(processor, retrochip::ids::genPatch)) == retrochip::GenesisEngine::kUserPatch);
        CHECK(juce::roundToInt(paramValue(processor, retrochip::ids::genAlgorithm)) == 5);
        CHECK(juce::roundToInt(paramValue(processor, retrochip::ids::genFeedback)) == 5);
        CHECK(box->getText() == "Lead"); // the menu shows the pick
        followProcessor();
        CHECK(labelById(*editor, "genStatus")->getText() == "Lead");
    }

    // Files added later appear when asked for, or when the console comes back into view.
    writeTfi(folder.dir.getChildFile("Sonic/Arp.tfi"));
    CHECK(box->getNumItems() == 2);
    click(*editor, "genRefresh");
    CHECK((menuRows(*box) == Rows { "# Sonic", "Arp", "Lead", "# Other", "Bass" }));
    writeTfi(folder.dir.getChildFile("Zap.tfi"));
    setConsole(processor, 0);
    setConsole(processor, 1);
    CHECK((menuRows(*box) == Rows { "# Sonic", "Arp", "Lead", "# Other", "Bass", "Zap" }));

    // A file gone since the scan is the import's to report.
    CHECK(folder.dir.getChildFile("Zap.tfi").deleteFile());
    if (choose(*box, "Zap"))
    {
        CHECK(processor.getUserPatchName() == "Lead");
        followProcessor();
        CHECK(labelById(*editor, "genStatus")->getText().contains("Zap.tfi"));
    }
}

void library_picker_says_when_the_folder_is_empty_or_cannot_be_made()
{
    TempFolder folder;
    RetroChipProcessor processor;
    const auto missing = folder.dir.getChildFile("missing");
    processor.setLibraryRoot(retrochip::Library::tfi, missing);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    setConsole(processor, 1);
    auto* box = comboById(*editor, "genLibrary");
    if (box == nullptr)
        return;
    CHECK(box->getNumItems() == 0);
    CHECK(box->getTextWhenNothingSelected() == "Empty folder");
    CHECK(box->getTextWhenNoChoicesAvailable() == "No timbre: add .tfi files to the folder");
    CHECK(box->getTooltip() == "No timbre: add .tfi files to the folder");

    // A file where the folder should be: it cannot be made, so nothing is shown in the Finder.
    const auto blocker = folder.dir.getChildFile("blocker");
    CHECK(blocker.replaceWithText("in the way"));
    processor.setLibraryRoot(retrochip::Library::tfi, blocker);
    click(*editor, "genOpenFolder");
    CHECK(box->getTextWhenNothingSelected() == "Folder cannot be created");
    CHECK(box->getTooltip() == "Folder cannot be created: blocker");
    CHECK(blocker.existsAsFile());
    click(*editor, "genRefresh");
    CHECK(box->getTextWhenNothingSelected() == "Empty folder");
}

void fti_folder_lists_its_instruments_and_sends_each_to_its_chip()
{
    TempFolder folder;
    writeFtiVrc6(inFolder(folder.dir, "Konami/Saw.fti"), "Saw");
    writeFti2a03(inFolder(folder.dir, "Lead.fti"), "Lead", { 15 });
    writeTfi(folder.dir.getChildFile("Konami/not.tfi"));

    RetroChipProcessor processor;
    processor.setLibraryRoot(retrochip::Library::fti, folder.dir);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    setConsole(processor, 0);
    CHECK(isShown(*editor, "nesLibraryPanel"));
    auto* box = comboById(*editor, "nesLibrary");
    if (box == nullptr)
        return;
    CHECK((menuRows(*box) == Rows { "# Konami", "Saw", "# Other", "Lead" }));
    CHECK(buttonById(*editor, "nesOpenFolder")->getTooltip() == "Opens the folder of .fti instruments, and creates it if it does not exist");

    if (choose(*box, "Saw"))
    {
        CHECK(juce::roundToInt(consoleValue(processor)) == kVrc6Console);
        CHECK(paramValue(processor, retrochip::ids::nesUseInstrument) > 0.5f);
        CHECK(processor.getNesInstrumentName(1) == "Saw");
        CHECK(box->getText() == "Saw");
    }
    CHECK(isShown(*editor, "nesLibraryPanel")); // on every NES chip, which share the folder
    setConsole(processor, kN163Console);
    CHECK(isShown(*editor, "nesLibraryPanel"));
    if (choose(*box, "Lead"))
    {
        CHECK(juce::roundToInt(consoleValue(processor)) == 0);
        CHECK(processor.getNesInstrumentName(0) == "Lead");
    }
    setConsole(processor, kSoundFontConsole);
    CHECK(!isShown(*editor, "nesLibraryPanel"));
    CHECK(!isShown(*editor, "genLibraryPanel"));
}

void language_is_english_until_the_user_picks_french()
{
    using retrochip::Language;
    const auto testsFile = retrochip::settingsFile();
    TempFolder folder;
    const auto file = folder.dir.getChildFile("RetroChip/Settings.xml");

    retrochip::useSettingsFile(file);
    CHECK(retrochip::currentLanguage() == Language::english);

    retrochip::setLanguage(Language::french);
    CHECK(retrochip::currentLanguage() == Language::french);
    CHECK(file.loadFileAsString().contains("language=\"fr\""));
    retrochip::useSettingsFile(file); // read again, as the next session would
    CHECK(retrochip::currentLanguage() == Language::french);

    for (const char* content : { "not xml", "<OTHER language=\"fr\"/>", "<RETROCHIP_SETTINGS language=\"de\"/>", "" })
    {
        CHECK(file.replaceWithText(content));
        retrochip::useSettingsFile(file);
        CHECK(retrochip::currentLanguage() == Language::english);
    }

    // The first processor reads the file, once: a later change of the file waits for the next session.
    CHECK(file.replaceWithText("<RETROCHIP_SETTINGS language=\"fr\"/>"));
    retrochip::resetLanguageForTests(file);
    CHECK(retrochip::currentLanguage() == Language::english);
    {
        RetroChipProcessor first;
        CHECK(retrochip::currentLanguage() == Language::french);
        CHECK(file.replaceWithText("<RETROCHIP_SETTINGS language=\"en\"/>"));
        RetroChipProcessor second;
        CHECK(retrochip::currentLanguage() == Language::french);
    }

    retrochip::useSettingsFile(testsFile);
    retrochip::setLanguage(Language::english);
    CHECK(retrochip::currentLanguage() == Language::english);
    CHECK(retrochip::settingsFile() == testsFile);
}

void french_table_is_consistent()
{
    std::set<std::string> keys;
    for (const auto& [english, french] : retrochip::frenchTable())
    {
        CHECK(keys.insert(english).second);
        const auto key = juce::String::fromUTF8(english);
        const auto value = juce::String::fromUTF8(french);
        CHECK(value.isNotEmpty());
        // French puts a space before a colon, and the pieces that are joined keep their spaces at the ends.
        if (key.endsWith(": ") && key != ": ")
            CHECK(value.endsWith(" : "));
        CHECK(key == ": " || key.startsWithChar(' ') == value.startsWithChar(' ')); // ": " is " : " in French
        CHECK(key.endsWithChar(' ') == value.endsWithChar(' '));
    }

    CHECK(retrochip::tr("Import") == "Import");
    {
        InFrench french;
        CHECK(retrochip::tr("Import") == "Importer");
        CHECK(retrochip::oneDecimal(3.6) == "3,6");
    }
    CHECK(retrochip::oneDecimal(3.6) == "3.6");
}

// Everything the editor's components say or hold for a tooltip, shown or not. The switch to French says
// "Français" in any language, so it is left out.
void collectTexts(juce::Component& component, juce::StringArray& texts)
{
    if (component.getComponentID() == "language_fr")
        return;
    if (auto* button = dynamic_cast<juce::Button*>(&component))
        texts.add(button->getButtonText());
    if (auto* label = dynamic_cast<juce::Label*>(&component))
        texts.add(label->getText());
    if (auto* box = dynamic_cast<juce::ComboBox*>(&component))
    {
        texts.add(box->getText());
        texts.add(box->getTextWhenNothingSelected());
        texts.add(box->getTextWhenNoChoicesAvailable());
        for (const auto& row : menuRows(*box))
            texts.add(row);
    }
    if (auto* client = dynamic_cast<juce::SettableTooltipClient*>(&component))
        texts.add(client->getTooltip());
    for (auto* child : component.getChildren())
        collectTexts(*child, texts);
}

bool looksFrench(const juce::String& text)
{
    if (text.containsAnyOf(juce::String::fromUTF8("éèàçêôûùœÉÈÀÇ")))
        return true;
    for (const char* word : { "le", "la", "les", "un", "une", "des", "du", "de", "et", "ou", "pour", "dans", "Importer", "Importe",
                              "Dossier", "Fichier", "Aucun", "Choisir", "Ouvrir", "Actualiser", "Autres", "Mon", "Infini" })
        if (text.containsWholeWord(word))
            return true;
    return false;
}

// Every console, every NES channel and every kind of Genesis timbre, with folders that have files in them.
juce::StringArray allTexts(RetroChipProcessor& processor, juce::Component& editor)
{
    juce::StringArray texts;
    const auto gather = [&]
    {
        followProcessor();
        collectTexts(editor, texts);
        texts.add(processor.getSampleStatus());
        texts.add(processor.getGenesisStatus());
        texts.add(processor.getNesStatus());
        texts.add(processor.getSoundFontStatus());
    };
    for (int console = 0; console < 7; ++console)
    {
        setConsole(processor, console);
        gather();
    }
    setConsole(processor, 0);
    for (int channel = 0; channel < 4; ++channel)
    {
        setParam(processor, retrochip::ids::channel, static_cast<float>(channel));
        gather();
    }
    setConsole(processor, 1);
    for (const int timbre : { retrochip::GenesisEngine::kUserPatch, retrochip::GenesisEngine::kPcmPatch, retrochip::GenesisEngine::kPcmKitPatch })
    {
        setParam(processor, retrochip::ids::genPatch, static_cast<float>(timbre));
        gather();
    }

    // What the host shows of the parameters.
    for (auto* parameter : processor.getParameters())
    {
        const int steps = juce::jlimit(2, 200, parameter->getNumSteps());
        for (int i = 0; i < steps; ++i)
            texts.add(parameter->getText(static_cast<float>(i) / static_cast<float>(steps - 1), 1024));
    }
    texts.removeEmptyStrings();
    return texts;
}

struct FilledFolders
{
    TempFolder folder;
    FilledFolders()
    {
        writeTfi(inFolder(folder.dir, "tfi/Sonic/Lead.tfi"));
        writeTfi(inFolder(folder.dir, "tfi/Bass.tfi"));
        writeFti2a03(inFolder(folder.dir, "fti/Lead.fti"), "Lead", { 15 });
        CHECK(folder.dir.getChildFile("sf2").createDirectory().wasOk());
    }
    void useIn(RetroChipProcessor& processor) const
    {
        processor.setLibraryRoot(retrochip::Library::tfi, folder.dir.getChildFile("tfi"));
        processor.setLibraryRoot(retrochip::Library::fti, folder.dir.getChildFile("fti"));
        processor.setLibraryRoot(folder.dir.getChildFile("sf2"));
    }
};

void english_interface_has_no_french_left()
{
    FilledFolders folders;
    RetroChipProcessor processor;
    folders.useIn(processor);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    const auto texts = allTexts(processor, *editor);
    CHECK(texts.size() > 100);
    for (const auto& text : texts)
        if (looksFrench(text))
        {
            std::printf("  FAILED: French in the English interface: %s\n", text.toRawUTF8());
            ++failures;
        }
    CHECK(texts.contains("# Other"));
    CHECK(texts.contains("No SoundFont: add .sf2 files to the folder"));
}

void french_interface_has_every_translation()
{
    FilledFolders folders;
    InFrench french;
    RetroChipProcessor processor;
    folders.useIn(processor);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    const auto texts = allTexts(processor, *editor);
    const auto missing = retrochip::missingTranslations();
    for (const auto& key : missing)
        std::printf("  FAILED: no French for: %s\n", key.toRawUTF8());
    CHECK(missing.isEmpty());
    for (const char* expected : { "Importer", "Ouvrir le dossier", "Actualiser", "Depuis le dossier", "# Autres", "Mon timbre TFI",
                                  "Mon échantillon PCM", "Mon kit PCM", "Mon échantillon", "Cordes", "Infini",
                                  "Importe un fichier .tfi pour jouer ce timbre",
                                  "Aucun SoundFont : ajoute des fichiers .sf2 dans le dossier" })
        if (!texts.contains(juce::String::fromUTF8(expected)))
        {
            std::printf("  FAILED: not shown in French: %s\n", expected);
            ++failures;
        }
}

void editors_follow_a_change_of_language()
{
    TempFolder folder;
    RetroChipProcessor processor;
    processor.setLibraryRoot(retrochip::Library::tfi, folder.dir.getChildFile("none"));
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    RetroChipProcessor other;
    std::unique_ptr<juce::AudioProcessorEditor> otherEditor(other.createEditor());
    setConsole(processor, 1);
    setParam(processor, retrochip::ids::genPatch, static_cast<float>(retrochip::GenesisEngine::kUserPatch));
    setConsole(other, kSoundFontConsole);

    auto* english = buttonById(*editor, "language_en");
    auto* french = buttonById(*editor, "language_fr");
    auto* timbres = comboById(*editor, "genPatch");
    auto* library = comboById(*editor, "genLibrary");
    if (english == nullptr || french == nullptr || timbres == nullptr || library == nullptr)
        return;
    CHECK(english->getToggleState() && !french->getToggleState());
    CHECK(textButtonById(*editor, "genImport")->getButtonText() == "Import");
    CHECK(labelById(*editor, "genStatus")->getText() == "Import a .tfi file to play this timbre");
    CHECK(timbres->getText() == "My TFI timbre");
    CHECK(library->getTextWhenNothingSelected() == "Empty folder");
    CHECK(buttonById(*editor, "tab_1")->getTooltip() == "Two chips: YM2612 (FM) and SN76489 (PSG)");

    french->onClick();
    CHECK(retrochip::currentLanguage() == retrochip::Language::french);
    CHECK(!english->getToggleState() && french->getToggleState());
    CHECK(textButtonById(*editor, "genImport")->getButtonText() == "Importer");
    CHECK(labelById(*editor, "genStatus")->getText() == "Importe un fichier .tfi pour jouer ce timbre");
    CHECK(timbres->getText() == "Mon timbre TFI"); // the choice is kept
    CHECK(juce::roundToInt(paramValue(processor, retrochip::ids::genPatch)) == retrochip::GenesisEngine::kUserPatch);
    CHECK(library->getTextWhenNothingSelected() == "Dossier vide");
    CHECK(library->getTooltip() == juce::String::fromUTF8("Aucun timbre : ajoute des fichiers .tfi dans le dossier"));
    CHECK(buttonById(*editor, "tab_1")->getTooltip() == "Deux puces : YM2612 (FM) et SN76489 (PSG)");
    CHECK(processor.apvts.getParameter(retrochip::ids::snesSustainRate)->getText(0.0f, 64) == "Infini");
    CHECK(textButtonById(*otherEditor, "sfRefresh")->getButtonText() == "Actualiser"); // every open editor
    CHECK(statusText(*otherEditor) == juce::String::fromUTF8("Aucun SoundFont : ajoute des fichiers .sf2 dans le dossier")
          || other.getLibraryRoot().isDirectory());
    saveSnapshot(*editor, "editor_ym2612_fr.png");

    english->onClick();
    CHECK(retrochip::currentLanguage() == retrochip::Language::english);
    CHECK(textButtonById(*editor, "genImport")->getButtonText() == "Import");
    CHECK(timbres->getText() == "My TFI timbre");
    CHECK(textButtonById(*otherEditor, "sfRefresh")->getButtonText() == "Refresh");
}

// The arrow keys.

bool press(juce::Component& editor, int keyCode, juce::ModifierKeys modifiers = {})
{
    return editor.keyPressed(juce::KeyPress(keyCode, modifiers, 0));
}

int programOf(RetroChipProcessor& processor) { return processor.getCurrentProgram(); }

void editor_takes_the_keyboard_for_the_arrows_only()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    CHECK(editor->getWantsKeyboardFocus());
    CHECK(processor.getNumPrograms() > 0);

    // Other keys, and arrows with a modifier, are the host's: space plays, a command key is its shortcut.
    CHECK(!press(*editor, juce::KeyPress::spaceKey));
    CHECK(!press(*editor, 'r'));
    CHECK(!press(*editor, juce::KeyPress::returnKey));
    CHECK(!press(*editor, juce::KeyPress::downKey, juce::ModifierKeys::commandModifier));
    CHECK(press(*editor, juce::KeyPress::downKey));

    // The on-screen keyboard never takes the keys, and a click on a control leaves them with the window.
    auto* keyboard = dynamic_cast<juce::MidiKeyboardComponent*>(findById(*editor, "keyboardComponent"));
    if (keyboard == nullptr)
        for (auto* child : editor->getChildren())
            if (auto* found = dynamic_cast<juce::MidiKeyboardComponent*>(child))
                keyboard = found;
    CHECK(keyboard != nullptr && !keyboard->getWantsKeyboardFocus());
    for (const char* id : { "genImport", "sfRefresh", "preset", "genPatch", "tab_0", "console_1", "channel_0", "genRefresh", "genLibrary" })
        if (auto* component = findById(*editor, id))
            CHECK(!component->getMouseClickGrabsKeyboardFocus());
        else
            CHECK(false);
}

void arrows_step_through_the_presets_of_a_chip()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    processor.setCurrentProgram(0);
    CHECK(press(*editor, juce::KeyPress::downKey));
    CHECK(programOf(processor) == 1);
    CHECK(press(*editor, juce::KeyPress::downKey));
    CHECK(programOf(processor) == 2);
    CHECK(press(*editor, juce::KeyPress::upKey));
    CHECK(programOf(processor) == 1);
    press(*editor, juce::KeyPress::upKey);
    press(*editor, juce::KeyPress::upKey);
    CHECK(programOf(processor) == 0); // the first stays the first
    for (int i = 0; i < 20; ++i)
        press(*editor, juce::KeyPress::downKey);
    CHECK(programOf(processor) == 11); // the 2A03's last, not the next chip's first
    CHECK(juce::roundToInt(consoleValue(processor)) == 0);

    // The PSG has neither instruments nor a folder: both pairs of arrows go through its presets.
    setConsole(processor, kPsgConsole);
    press(*editor, juce::KeyPress::rightKey);
    const int first = programOf(processor);
    CHECK(retrochip::kPresets[static_cast<size_t>(first)].console == kPsgConsole);
    press(*editor, juce::KeyPress::downKey);
    CHECK(programOf(processor) == first + 1);
    press(*editor, juce::KeyPress::leftKey);
    CHECK(programOf(processor) == first);
    CHECK(juce::roundToInt(consoleValue(processor)) == kPsgConsole);
}

void arrows_step_through_fm_timbres_and_the_tfi_folder()
{
    TempFolder folder;
    writeTfi(inFolder(folder.dir, "A.tfi"), 1, 1);
    writeTfi(inFolder(folder.dir, "B.tfi"), 2, 2);
    RetroChipProcessor processor;
    processor.setLibraryRoot(retrochip::Library::tfi, folder.dir);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    setConsole(processor, 1);
    setParam(processor, retrochip::ids::genPatch, 0.0f);
    const auto timbre = [&] { return juce::roundToInt(paramValue(processor, retrochip::ids::genPatch)); };

    press(*editor, juce::KeyPress::downKey);
    CHECK(timbre() == 1);
    CHECK(juce::roundToInt(paramValue(processor, retrochip::ids::genAlgorithm)) == retrochip::kFmPatches[1].algorithm + 1);

    // Past the last built-in timbre are the user's, which are passed by while they have nothing.
    for (int i = 0; i < 20; ++i)
        press(*editor, juce::KeyPress::downKey);
    CHECK(timbre() == retrochip::GenesisEngine::kUserPatch - 1);

    // Right and left go through the folder: each step imports the file.
    press(*editor, juce::KeyPress::rightKey);
    CHECK(processor.getUserPatchName() == "A");
    CHECK(timbre() == retrochip::GenesisEngine::kUserPatch);
    press(*editor, juce::KeyPress::rightKey);
    CHECK(processor.getUserPatchName() == "B");
    CHECK(juce::roundToInt(paramValue(processor, retrochip::ids::genAlgorithm)) == 3);
    press(*editor, juce::KeyPress::rightKey);
    CHECK(processor.getUserPatchName() == "B"); // the last stays the last
    press(*editor, juce::KeyPress::leftKey);
    CHECK(processor.getUserPatchName() == "A");
    CHECK(comboById(*editor, "genLibrary")->getText() == "A");

    // A built-in timbre: the folder's menu no longer names the sound, and right starts from the first file again.
    press(*editor, juce::KeyPress::upKey);
    CHECK(timbre() == retrochip::GenesisEngine::kUserPatch - 1);
    CHECK(comboById(*editor, "genLibrary")->getSelectedId() == 0);
    press(*editor, juce::KeyPress::downKey);
    CHECK(timbre() == retrochip::GenesisEngine::kUserPatch); // the imported one is there now
    press(*editor, juce::KeyPress::rightKey);
    CHECK(processor.getUserPatchName() == "A");
}

void arrows_step_through_snes_instruments()
{
    RetroChipProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    setConsole(processor, 2);
    setParam(processor, retrochip::ids::snesInstrument, 10.0f);
    const auto instrument = [&] { return juce::roundToInt(paramValue(processor, retrochip::ids::snesInstrument)); };
    press(*editor, juce::KeyPress::downKey);
    CHECK(instrument() == 11);
    press(*editor, juce::KeyPress::downKey);
    CHECK(instrument() == 11); // "My sample" has nothing yet
    press(*editor, juce::KeyPress::upKey);
    CHECK(instrument() == 10);

    // Left and right go through the S-DSP's presets.
    press(*editor, juce::KeyPress::rightKey);
    CHECK(retrochip::kPresets[static_cast<size_t>(programOf(processor))].console == 2);
}

void arrows_step_through_soundfont_files_and_instruments()
{
    TempFolder folder;
    testsf2::TestSf2Options options;
    options.presets = { { "First", 0, 0 }, { "Second", 0, 3 } };
    writeSf2(inFolder(folder.dir, "Group/one.sf2"), options);
    writeSf2(inFolder(folder.dir, "Group/two.sf2"));

    RetroChipProcessor processor;
    processor.setLibraryRoot(folder.dir);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    setConsole(processor, kSoundFontConsole);

    press(*editor, juce::KeyPress::rightKey);
    waitForLoad(processor);
    followProcessor();
    CHECK(processor.getSoundFontPath() == "Group/one.sf2");
    CHECK(processor.getSoundFontInstrumentIndex() == 0);

    press(*editor, juce::KeyPress::downKey);
    waitForLoad(processor);
    followProcessor();
    CHECK(processor.getSoundFontInstrumentIndex() == 1);
    press(*editor, juce::KeyPress::downKey);
    waitForLoad(processor);
    CHECK(processor.getSoundFontInstrumentIndex() == 1);
    press(*editor, juce::KeyPress::upKey);
    waitForLoad(processor);
    CHECK(processor.getSoundFontInstrumentIndex() == 0);

    press(*editor, juce::KeyPress::rightKey);
    waitForLoad(processor);
    followProcessor();
    CHECK(processor.getSoundFontPath() == "Group/two.sf2");
    press(*editor, juce::KeyPress::leftKey);
    waitForLoad(processor);
    CHECK(processor.getSoundFontPath() == "Group/one.sf2");
}

void arrows_step_through_the_fti_folder()
{
    TempFolder folder;
    writeFti2a03(inFolder(folder.dir, "a.fti"), "Lead", { 15 });
    writeFtiVrc6(inFolder(folder.dir, "b.fti"), "Saw");
    RetroChipProcessor processor;
    processor.setLibraryRoot(retrochip::Library::fti, folder.dir);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    setConsole(processor, 0);

    press(*editor, juce::KeyPress::rightKey);
    CHECK(processor.getNesInstrumentName(0) == "Lead");
    press(*editor, juce::KeyPress::rightKey);
    CHECK(processor.getNesInstrumentName(1) == "Saw");
    CHECK(juce::roundToInt(consoleValue(processor)) == kVrc6Console); // the instrument brought its chip
    press(*editor, juce::KeyPress::leftKey);
    CHECK(juce::roundToInt(consoleValue(processor)) == 0);
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    // The tests choose the language themselves, in a file of their own: the user's settings are never touched.
    const auto settings = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("RetroChipSettings", ".xml");
    retrochip::useSettingsFile(settings);

    if (argc > 1 && std::strcmp(argv[1], "--write-references") == 0)
        return writeReferences();

    midi_note_produces_sound();
    note_off_ends_sound();
    pitch_wheel_and_sustain_reach_the_engine();
    there_are_twelve_2a03_presets_that_all_sound();
    state_round_trip_keeps_values_and_program();
    corrupt_state_keeps_current_values();
    partial_state_uses_defaults_for_missing_parameters();
    preset_change_with_held_notes_is_safe();
    empty_and_large_blocks_are_safe();
    sample_rate_change_is_safe();
    gain_scales_output();
    reports_latency_and_formats();
    editor_has_fixed_size_and_titles();
    editor_greys_out_controls_by_channel();
    editor_controls_change_parameters();
    nes_render_matches_references();
    nes_state_loads_as_2a03();
    old_parameter_ids_are_unchanged();
    console_change_stops_held_notes();
    console_change_clears_sustain_and_pitch_bend();
    console_selection_picks_engine_and_latency();
    console_selector_shows_chip_names();
    editor_shows_2a03_panels_only_on_2a03();
    ym2612_note_produces_sound();
    ym2612_presets_sound();
    ym2612_preset_sets_shared_parameters();
    ym2612_defaults_match_the_spec();
    ym2612_parameters_reach_the_engine();
    ym2612_plugin_drives_the_engine_with_the_parameters();
    ym2612_parameters_do_not_reach_the_2a03();
    ym2612_state_round_trip();
    reported_latency_follows_console();
    ym2612_timbre_selection_copies_algorithm_and_feedback();
    ym2612_panel_shows_only_its_controls();
    ym2612_snapshot();
    console_switch_with_held_chip_notes_is_safe();
    sdsp_note_produces_sound();
    thirty_six_presets_all_sound();
    sdsp_preset_sets_its_parameters();
    preset_changes_console_with_held_notes();
    sdsp_defaults_match_the_spec();
    sdsp_knobs_show_chip_times();
    sdsp_parameters_reach_the_engine();
    sdsp_plugin_drives_the_engine_with_the_parameters();
    sdsp_instruments_share_one_bank();
    sdsp_instrument_change_stops_held_notes();
    sdsp_state_round_trip();
    sdsp_panel_shows_only_its_controls();
    sdsp_instrument_menu_changes_the_parameter();
    no_sample_prompts_to_import();
    import_valid_wav_plays();
    import_preserves_pitch_at_every_rate();
    import_stereo_is_mixed_to_mono();
    import_various_formats();
    import_peak_is_limited_without_normalising();
    import_keeps_the_end_of_the_sample();
    import_looped_sine_holds_its_level();
    import_loops_close_at_any_length();
    import_invalid_keeps_current();
    import_rejects_implausible_headers();
    import_too_long_is_truncated();
    user_sample_round_trips_in_state();
    corrupt_sample_in_state_keeps_rest();
    replacing_sample_during_play_is_safe();
    replacing_sample_while_audio_runs();
    loop_setting_applies_to_user_sample();
    editor_import_panel_shows_the_status();
    editor_import_selects_the_user_sample();
    sdsp_snapshot();
    return_to_a_console_has_no_stale_tail();
    first_note_after_a_return_does_not_glide();
    pedal_and_bend_follow_into_the_new_console();
    prepare_clears_pedal_and_bend();
    preset_menu_is_empty_on_another_console();
    preset_menu_timer_leaves_a_pick_in_progress();
    typed_values_are_read_as_shown();
    status_tooltip_has_a_window_to_appear_in();
    vibrato_knobs_are_named_in_full();
    nes_state_loads_into_a_running_sdsp_instance();
    library_groups_and_sorts();
    library_missing_folder_gives_a_message();
    unreadable_folder_gives_a_message();
    library_handles_accented_names();
    resolve_rejects_paths_leaving_the_root();
    library_survives_a_symlink_loop();
    library_default_is_in_the_music_folder();
    soundfont_parameters_match_the_spec();
    soundfont_loads_and_plays();
    instrument_choice_changes_the_sound();
    a_new_file_starts_on_its_first_instrument();
    instrument_names_decode_as_utf8_or_latin1();
    load_refuses_unreadable_and_too_large();
    last_request_wins();
    bank_built_before_prepare_plays_at_the_right_rate();
    adopting_a_bank_allocates_nothing();
    replaced_bank_waits_for_the_audio_thread();
    state_round_trips_file_and_instrument();
    state_with_missing_file_is_silent_with_message();
    state_with_unreadable_file_keeps_the_sound_or_the_reference();
    state_with_missing_instrument_uses_the_first();
    retry_loads_a_file_that_came_back();
    state_with_escaping_path_is_treated_as_missing();
    state_keeps_a_load_in_progress();
    old_state_opens_without_soundfont();
    deleted_file_keeps_playing_until_reopen();
    console_change_stops_soundfont_notes();
    reported_latency_is_zero_on_soundfont();
    soundfont_echo_and_velocity_reach_the_engine();
    replacing_soundfont_while_audio_runs();
    destruction_during_a_load_is_safe();
    loader_delivers_the_last_request_on_its_own_thread();
    soundfont_panel_shows_only_its_controls();
    console_selector_fits_every_segment();
    file_menu_has_a_heading_per_group();
    choosing_a_file_loads_it_and_lists_instruments();
    choosing_an_instrument_changes_the_sound();
    refresh_picks_up_new_files();
    status_shows_missing_file();
    refresh_reloads_a_file_that_came_back();
    preset_menu_is_empty_on_soundfont();
    library_folder_is_created_for_the_button();
    soundfont_snapshot();
    genesis_timbre_menu_ends_with_the_imports();
    tfi_import_plays_as_the_user_patch();
    genesis_import_refuses_what_it_cannot_use();
    pcm_import_makes_8_bit_samples_at_the_dac_rate();
    pcm_import_cuts_what_is_too_long();
    pcm_import_plays_on_the_root_note();
    pcm_loop_parameter_reaches_the_engine();
    each_genesis_import_keeps_the_other();
    genesis_imports_round_trip_in_the_state();
    damaged_genesis_state_is_silent_with_a_message();
    replaced_genesis_sounds_wait_for_the_audio_thread();
    replacing_genesis_sound_while_audio_runs();
    editor_genesis_import_selects_what_was_imported();
    ym2612_panel_follows_the_timbre();
    kit_import_puts_the_files_on_the_keys_in_name_order();
    kit_import_skips_what_it_cannot_use();
    kit_import_keeps_to_its_limits();
    kit_plays_through_the_processor();
    kit_round_trips_in_the_state_and_keeps_the_other_imports();
    damaged_kit_in_the_state_keeps_what_it_can();
    editor_import_picks_the_kit_from_the_selection();
    ym2612_panel_with_the_kit();
    psg_is_a_console_with_its_engine_and_latency();
    psg_parameters_reach_the_engine();
    psg_presets_set_their_controls();
    psg_settings_round_trip_in_the_state();
    psg_panel_shows_only_its_controls();
    preset_menu_lists_the_psg_after_the_others();
    value_boxes_keep_their_outline_across_consoles();
    nes_chips_are_consoles_on_the_nes_engine();
    cartridge_chip_parameters_reach_the_engine();
    dpcm_wav_import_plays_on_its_root_note();
    fti_import_brings_the_instrument_to_its_chip();
    fti_import_refuses_what_it_cannot_play();
    fti_kit_plays_its_samples_on_the_dpcm_channel();
    nes_imports_round_trip_in_the_state();
    replaced_nes_sounds_wait_for_the_audio_thread();
    cartridge_presets_set_their_chip();
    nes_panels_follow_the_chip_and_channel();
    editor_nes_import_picks_the_chip_and_channel();
    library_folders_are_under_the_music_folder();
    scan_keeps_the_library_extension();
    tfi_folder_lists_its_timbres_and_imports_one();
    library_picker_says_when_the_folder_is_empty_or_cannot_be_made();
    fti_folder_lists_its_instruments_and_sends_each_to_its_chip();
    language_is_english_until_the_user_picks_french();
    french_table_is_consistent();
    english_interface_has_no_french_left();
    french_interface_has_every_translation();
    editors_follow_a_change_of_language();
    editor_takes_the_keyboard_for_the_arrows_only();
    arrows_step_through_the_presets_of_a_chip();
    arrows_step_through_fm_timbres_and_the_tfi_folder();
    arrows_step_through_snes_instruments();
    arrows_step_through_soundfont_files_and_instruments();
    arrows_step_through_the_fti_folder();

    settings.deleteFile();
    std::printf("plugin tests: %d failed checks\n", failures);
    return failures == 0 ? 0 : 1;
}
