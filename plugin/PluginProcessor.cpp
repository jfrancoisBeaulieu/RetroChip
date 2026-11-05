// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "PluginProcessor.h"

#include "Localisation.h"
#include "Parameters.h"
#include "PluginEditor.h"
#include "Presets.h"
#include "SampleImporter.h"
#include "SoundFontLibrary.h"
#include "nes/Dpcm.h"

#include <algorithm>

namespace retrochip
{
namespace
{
constexpr double kPitchBendRangeSemitones = 2.0;
constexpr const char* kProgramProperty = "program";
constexpr const char* kSfPathProperty = "sfPath";
constexpr const char* kSfBankProperty = "sfBank";
constexpr const char* kSfProgramProperty = "sfProgram";
constexpr const char* kSfNameProperty = "sfName";
constexpr int kRetiredBankPollMs = 250;

// Stands for "nothing new to adopt" and "nothing adopted yet"; a null pointer is a real choice, no sample.
const SnesSample* noNewSample()
{
    static const SnesSample marker;
    return &marker;
}

const GenesisUserSound* noNewSound()
{
    static const GenesisUserSound marker;
    return &marker;
}

const NesImports* noNewNes()
{
    static const NesImports marker;
    return &marker;
}

// The NES's consoles, as NesChip indexes; -1 for the others.
int nesChipOf(Console console)
{
    switch (console)
    {
        case Console::Chip2A03: return static_cast<int>(NesChip::Ricoh2A03);
        case Console::Vrc6: return static_cast<int>(NesChip::Vrc6);
        case Console::N163: return static_cast<int>(NesChip::N163);
        case Console::Ym2612:
        case Console::SDsp:
        case Console::SoundFont:
        case Console::Psg: return -1;
    }
    return -1;
}

// The same for banks, which cannot be built empty. It is only ever compared.
SoundFontBank* noNewBank()
{
    static char marker;
    return reinterpret_cast<SoundFontBank*>(&marker);
}

juce::String loadFailureText(SoundFontLoader::Failure failure, const juce::String& fileName)
{
    switch (failure)
    {
        case SoundFontLoader::Failure::missing: return tr("File not found: ") + fileName;
        case SoundFontLoader::Failure::unreadable: return tr("Unreadable file: ") + fileName;
        case SoundFontLoader::Failure::tooLarge: return tr("File too large (1 GB limit): ") + fileName;
        case SoundFontLoader::Failure::none: break;
    }
    return {};
}
} // namespace

int RetroChipProcessor::latencyFor(Console which) const
{
    switch (which)
    {
        case Console::Chip2A03: return BandLimitedBuffer::kLatencySamples;
        case Console::Ym2612: return genesisEngine->latencySamples();
        case Console::SDsp: return snesEngine->latencySamples();
        case Console::SoundFont: return soundFontEngine->latencySamples();
        case Console::Psg: return psgEngine->latencySamples();
        case Console::Vrc6:
        case Console::N163: return BandLimitedBuffer::kLatencySamples;
    }
    return 0;
}

RetroChipProcessor::RetroChipProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "RetroChipState", createParameterLayout()),
      channel(apvts.getRawParameterValue(ids::channel)),
      duty(apvts.getRawParameterValue(ids::duty)),
      noiseMode(apvts.getRawParameterValue(ids::noiseMode)),
      attack(apvts.getRawParameterValue(ids::attack)),
      decay(apvts.getRawParameterValue(ids::decay)),
      sustain(apvts.getRawParameterValue(ids::sustain)),
      release(apvts.getRawParameterValue(ids::release)),
      vibratoRate(apvts.getRawParameterValue(ids::vibratoRate)),
      vibratoDepth(apvts.getRawParameterValue(ids::vibratoDepth)),
      glide(apvts.getRawParameterValue(ids::glide)),
      velocity(apvts.getRawParameterValue(ids::velocity)),
      gain(apvts.getRawParameterValue(ids::gain)),
      console(apvts.getRawParameterValue(ids::console)),
      genPatch(apvts.getRawParameterValue(ids::genPatch)),
      genAlgorithm(apvts.getRawParameterValue(ids::genAlgorithm)),
      genFeedback(apvts.getRawParameterValue(ids::genFeedback)),
      genBrightness(apvts.getRawParameterValue(ids::genBrightness)),
      genAttack(apvts.getRawParameterValue(ids::genAttack)),
      genDecay(apvts.getRawParameterValue(ids::genDecay)),
      genRelease(apvts.getRawParameterValue(ids::genRelease)),
      genRootNote(apvts.getRawParameterValue(ids::genRootNote)),
      genLoop(apvts.getRawParameterValue(ids::genLoop)),
      snesInstrument(apvts.getRawParameterValue(ids::snesInstrument)),
      snesAttack(apvts.getRawParameterValue(ids::snesAttack)),
      snesDecay(apvts.getRawParameterValue(ids::snesDecay)),
      snesSustainLevel(apvts.getRawParameterValue(ids::snesSustainLevel)),
      snesSustainRate(apvts.getRawParameterValue(ids::snesSustainRate)),
      snesRelease(apvts.getRawParameterValue(ids::snesRelease)),
      snesEchoMix(apvts.getRawParameterValue(ids::snesEchoMix)),
      snesEchoDelay(apvts.getRawParameterValue(ids::snesEchoDelay)),
      snesEchoFeedback(apvts.getRawParameterValue(ids::snesEchoFeedback)),
      snesRootNote(apvts.getRawParameterValue(ids::snesRootNote)),
      snesLoop(apvts.getRawParameterValue(ids::snesLoop)),
      sfEchoMix(apvts.getRawParameterValue(ids::sfEchoMix)),
      sfEchoDelay(apvts.getRawParameterValue(ids::sfEchoDelay)),
      sfEchoFeedback(apvts.getRawParameterValue(ids::sfEchoFeedback)),
      psgChannel(apvts.getRawParameterValue(ids::psgChannel)),
      psgNoiseType(apvts.getRawParameterValue(ids::psgNoiseType)),
      psgNoiseRate(apvts.getRawParameterValue(ids::psgNoiseRate)),
      psgAttack(apvts.getRawParameterValue(ids::psgAttack)),
      psgDecay(apvts.getRawParameterValue(ids::psgDecay)),
      psgSustain(apvts.getRawParameterValue(ids::psgSustain)),
      psgRelease(apvts.getRawParameterValue(ids::psgRelease)),
      nesUseInstrument(apvts.getRawParameterValue(ids::nesUseInstrument)),
      vrc6Channel(apvts.getRawParameterValue(ids::vrc6Channel)),
      vrc6Duty(apvts.getRawParameterValue(ids::vrc6Duty)),
      n163Wave(apvts.getRawParameterValue(ids::n163Wave)),
      n163Channels(apvts.getRawParameterValue(ids::n163Channels)),
      dpcmRootNote(apvts.getRawParameterValue(ids::dpcmRootNote)),
      dpcmLoop(apvts.getRawParameterValue(ids::dpcmLoop)),
      incomingSample(noNewSample()),
      adoptedSample(noNewSample()),
      incomingSound(noNewSound()),
      adoptedSound(noNewSound()),
      incomingNes(noNewNes()),
      adoptedNes(noNewNes()),
      libraryRoot(defaultLibraryRoot()),
      incomingBank(noNewBank()),
      adoptedBank(noNewBank()),
      soundFontLoader([this](SoundFontLoader::Result result) { receiveLoad(std::move(result)); })
{
    loadLanguage(); // before any text is shown, here on the message thread
    genesisEngine = std::make_unique<GenesisEngine>();

    // The bank is built once per process and shared, on this thread (the parameter layout has already
    // asked for it). The engine only reads it. The user's sample is set separately, from the import.
    snesEngine = std::make_unique<SnesEngine>();
    snesEngine->setBank(&sharedSnesBank());

    soundFontEngine = std::make_unique<SoundFontEngine>();
    psgEngine = std::make_unique<PsgEngine>();

    apvts.addParameterListener(ids::console, this);
}

RetroChipProcessor::~RetroChipProcessor()
{
    apvts.removeParameterListener(ids::console, this);

    // The loader first, as it may be about to call back. It stops without a message loop, and what it finished
    // and nobody took goes with the rest.
    soundFontLoader.stop();
    cancelPendingUpdate();
    stopTimer();
    // The samples and the banks go with the processor, which is after the host has stopped calling processBlock.
}

void RetroChipProcessor::parameterChanged(const juce::String&, float)
{
    triggerAsyncUpdate();
}

void RetroChipProcessor::handleAsyncUpdate()
{
    setLatencySamples(latencyFor(getActiveConsole()));
    takeFinishedLoad();
}

Console RetroChipProcessor::getActiveConsole() const
{
    const int index = juce::jlimit(0, static_cast<int>(Console::N163), juce::roundToInt(console->load()));
    return static_cast<Console>(index);
}

ConsoleEngine* RetroChipProcessor::engineFor(Console which)
{
    switch (which)
    {
        case Console::Chip2A03: return &nesEngine;
        case Console::Ym2612: return genesisEngine.get();
        case Console::SDsp: return snesEngine.get();
        case Console::SoundFont: return soundFontEngine.get();
        case Console::Psg: return psgEngine.get();
        case Console::Vrc6:
        case Console::N163: return &nesEngine;
    }
    return nullptr;
}

void RetroChipProcessor::prepareToPlay(double sampleRate, int)
{
    if (sampleRate > 0.0)
        knownSampleRate.store(sampleRate);
    nesEngine.setParams(readParams());
    nesEngine.prepare(sampleRate);
    genesisEngine->setParams(readGenesisParams());
    genesisEngine->prepare(sampleRate);
    snesEngine->setParams(readSnesParams());
    snesEngine->prepare(sampleRate);
    soundFontEngine->setParams(readSoundFontParams());
    soundFontEngine->prepare(sampleRate); // the bank in use follows the rate; a later one does so on adoption
    psgEngine->setParams(readPsgParams());
    psgEngine->prepare(sampleRate);
    keyboardState.reset();
    heldBend = 0.0;
    pedalDown = false;
    previousGain = juce::Decibels::decibelsToGain(gain->load());
    renderConsole = getActiveConsole();
    setLatencySamples(latencyFor(renderConsole));
}

bool RetroChipProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& output = layouts.getMainOutputChannelSet();
    return layouts.getMainInputChannelSet().isDisabled()
           && (output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo());
}

NesParams RetroChipProcessor::readParams() const
{
    NesParams params;
    params.channel = static_cast<NesChannel>(juce::jlimit(0, 3, juce::roundToInt(channel->load())));
    params.duty = static_cast<int>(duty->load());
    params.noiseShort = noiseMode->load() >= 0.5f;
    params.attackMs = attack->load();
    params.decayMs = decay->load();
    params.sustain = sustain->load();
    params.releaseMs = release->load();
    params.vibratoRateHz = vibratoRate->load();
    params.vibratoDepthCents = vibratoDepth->load();
    params.glideMs = glide->load();
    params.velocityOn = velocity->load() >= 0.5f;

    const int chip = nesChipOf(getActiveConsole());
    params.chip = static_cast<NesChip>(std::max(chip, 0));
    params.vrc6Saw = vrc6Channel->load() >= 0.5f;
    params.vrc6Duty = juce::roundToInt(vrc6Duty->load());
    params.n163Wave = juce::roundToInt(n163Wave->load());
    params.n163Channels = juce::roundToInt(n163Channels->load());
    params.dpcmRootNote = juce::roundToInt(dpcmRootNote->load());
    params.dpcmLoop = dpcmLoop->load() >= 0.5f;
    params.useInstrument = nesUseInstrument->load() >= 0.5f;
    return params;
}

GenesisParams RetroChipProcessor::readGenesisParams() const
{
    GenesisParams params;
    params.patch = juce::roundToInt(genPatch->load());
    params.algorithm = juce::roundToInt(genAlgorithm->load()) - 1; // shown from 1, counted from 0
    params.feedback = juce::roundToInt(genFeedback->load());
    params.brightness = genBrightness->load() / 100.0f;
    params.attack = genAttack->load() / 100.0f;
    params.decay = genDecay->load() / 100.0f;
    params.release = genRelease->load() / 100.0f;
    params.pcmRootNote = juce::roundToInt(genRootNote->load());
    params.pcmLoop = genLoop->load() >= 0.5f;
    params.vibratoRateHz = vibratoRate->load();
    params.vibratoDepthCents = vibratoDepth->load();
    params.glideMs = glide->load();
    params.velocityOn = velocity->load() >= 0.5f;
    return params;
}

SnesParams RetroChipProcessor::readSnesParams() const
{
    SnesParams params;
    params.instrument = juce::roundToInt(snesInstrument->load());
    params.attack = juce::roundToInt(snesAttack->load());
    params.decay = juce::roundToInt(snesDecay->load());
    params.sustainLevel = juce::roundToInt(snesSustainLevel->load());
    params.sustainRate = juce::roundToInt(snesSustainRate->load());
    params.releaseMs = snesRelease->load();
    params.echoMix = snesEchoMix->load() / 100.0f;
    params.echoDelay = juce::roundToInt(snesEchoDelay->load());
    params.echoFeedback = snesEchoFeedback->load() / 100.0f;
    params.vibratoRateHz = vibratoRate->load();
    params.vibratoDepthCents = vibratoDepth->load();
    params.glideMs = glide->load();
    params.velocityOn = velocity->load() >= 0.5f;
    params.rootNote = juce::roundToInt(snesRootNote->load());
    params.loop = snesLoop->load() >= 0.5f;
    return params;
}

SoundFontParams RetroChipProcessor::readSoundFontParams() const
{
    SoundFontParams params;
    params.instrument = soundFontInstrument.load();
    params.velocityOn = velocity->load() >= 0.5f;
    params.echoMix = sfEchoMix->load() / 100.0f;
    params.echoDelay = juce::roundToInt(sfEchoDelay->load());
    params.echoFeedback = sfEchoFeedback->load() / 100.0f;
    return params;
}

PsgParams RetroChipProcessor::readPsgParams() const
{
    PsgParams params;
    params.noise = psgChannel->load() >= 0.5f;
    params.periodic = psgNoiseType->load() >= 0.5f;
    params.noiseRate = juce::roundToInt(psgNoiseRate->load());
    params.attackMs = psgAttack->load();
    params.decayMs = psgDecay->load();
    params.sustain = juce::roundToInt(psgSustain->load());
    params.releaseMs = psgRelease->load();
    params.vibratoRateHz = vibratoRate->load();
    params.vibratoDepthCents = vibratoDepth->load();
    params.glideMs = glide->load();
    params.velocityOn = velocity->load() >= 0.5f;
    return params;
}

void RetroChipProcessor::handleMidi(ConsoleEngine& engine, const juce::MidiMessage& message)
{
    if (message.isNoteOn())
        engine.noteOn(message.getNoteNumber(), message.getVelocity());
    else if (message.isNoteOff())
        engine.noteOff(message.getNoteNumber());
    else if (message.isPitchWheel())
    {
        heldBend = (message.getPitchWheelValue() - 8192) / 8192.0 * kPitchBendRangeSemitones;
        engine.setPitchBend(heldBend);
    }
    else if (message.isSustainPedalOn())
    {
        pedalDown = true;
        engine.setSustain(true);
    }
    else if (message.isSustainPedalOff())
    {
        pedalDown = false;
        engine.setSustain(false);
    }
    else if (message.isAllNotesOff() || message.isAllSoundOff())
        engine.allNotesOff();
}

void RetroChipProcessor::adoptPublishedSample()
{
    const auto* next = incomingSample.exchange(noNewSample());
    if (next == noNewSample())
        return;
    snesEngine->setUserSample(next); // stops the voices that play the old one
    adoptedSample.store(next);
}

void RetroChipProcessor::adoptPublishedSound()
{
    const auto* next = incomingSound.exchange(noNewSound());
    if (next == noNewSound())
        return;
    genesisEngine->setUserSound(next); // stops the voices that play the old one
    adoptedSound.store(next);
}

void RetroChipProcessor::adoptPublishedNes()
{
    const auto* next = incomingNes.exchange(noNewNes());
    if (next == noNewNes())
        return;
    nesEngine.setUserSound(next != nullptr ? &next->sound : nullptr); // stops the notes
    adoptedNes.store(next);
}

void RetroChipProcessor::adoptPublishedBank()
{
    auto* next = incomingBank.exchange(noNewBank());
    if (next == noNewBank())
        return;
    soundFontEngine->setParams(readSoundFontParams()); // so the new bank starts on its own instrument
    soundFontEngine->setBank(next);                    // stops the notes of the old bank and of this one
    adoptedBank.store(next);
}

void RetroChipProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    adoptPublishedSample();
    adoptPublishedSound();
    adoptPublishedNes();
    adoptPublishedBank();

    const int numSamples = buffer.getNumSamples();
    const int numChannels = getTotalNumOutputChannels();
    if (numSamples == 0 || numChannels == 0)
        return;

    const auto selected = getActiveConsole();
    if (selected != renderConsole)
    {
        // allNotesOff() leaves the pedal and the bend, which would wait for the console to come back.
        if (auto* leaving = engineFor(renderConsole))
        {
            leaving->allNotesOff();
            leaving->setSustain(false);
            leaving->setPitchBend(0.0);
        }
        renderConsole = selected;

        // The player's wheel and pedal stay where they are, and the console being entered goes by them.
        if (auto* entering = engineFor(renderConsole))
        {
            entering->setPitchBend(heldBend);
            entering->setSustain(pedalDown);
        }
    }

    keyboardState.processNextMidiBuffer(midi, 0, numSamples, true);

    auto* engine = engineFor(renderConsole);
    if (engine == nullptr)
    {
        buffer.clear();
        previousGain = juce::Decibels::decibelsToGain(gain->load());
        midi.clear();
        return;
    }

    if (nesChipOf(renderConsole) >= 0)
        nesEngine.setParams(readParams());
    else if (renderConsole == Console::Ym2612)
        genesisEngine->setParams(readGenesisParams());
    else if (renderConsole == Console::SDsp)
        snesEngine->setParams(readSnesParams());
    else if (renderConsole == Console::Psg)
        psgEngine->setParams(readPsgParams());
    else
        soundFontEngine->setParams(readSoundFontParams());

    auto* out = buffer.getWritePointer(0);
    int position = 0;
    for (const auto metadata : midi)
    {
        const int eventPosition = juce::jlimit(position, numSamples, metadata.samplePosition);
        engine->render(out + position, eventPosition - position);
        position = eventPosition;
        handleMidi(*engine, metadata.getMessage());
    }
    engine->render(out + position, numSamples - position);

    const float newGain = juce::Decibels::decibelsToGain(gain->load());
    buffer.applyGainRamp(0, 0, numSamples, previousGain, newGain);
    previousGain = newGain;

    for (int ch = 1; ch < numChannels; ++ch)
        buffer.copyFrom(ch, 0, buffer, 0, 0, numSamples);

    midi.clear();
}

juce::AudioProcessorEditor* RetroChipProcessor::createEditor()
{
    return new RetroChipEditor(*this);
}

int RetroChipProcessor::getNumPrograms()
{
    return static_cast<int>(kPresets.size());
}

void RetroChipProcessor::setCurrentProgram(int index)
{
    if (index < 0 || index >= getNumPrograms())
        return;

    const auto& preset = kPresets[static_cast<size_t>(index)];
    const auto set = [this](const char* id, float value)
    {
        auto* parameter = apvts.getParameter(id);
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };

    set(ids::console, static_cast<float>(preset.console));

    // Each preset sets its own console's controls; the other console keeps what it had.
    if (preset.console == static_cast<int>(Console::Chip2A03))
    {
        set(ids::channel, static_cast<float>(preset.channel));
        set(ids::duty, static_cast<float>(preset.duty));
        set(ids::noiseMode, static_cast<float>(preset.noiseMode));
        set(ids::attack, preset.attack);
        set(ids::decay, preset.decay);
        set(ids::sustain, preset.sustain);
        set(ids::release, preset.release);
        set(ids::nesUseInstrument, 0.0f);
    }
    else if (preset.console == static_cast<int>(Console::Vrc6) || preset.console == static_cast<int>(Console::N163))
    {
        // The envelope is the 2A03's. A preset is played with it, not with an imported instrument.
        set(ids::attack, preset.attack);
        set(ids::decay, preset.decay);
        set(ids::sustain, preset.sustain);
        set(ids::release, preset.release);
        set(ids::nesUseInstrument, 0.0f);
        if (preset.console == static_cast<int>(Console::Vrc6))
        {
            set(ids::vrc6Channel, static_cast<float>(preset.vrc6Channel));
            set(ids::vrc6Duty, static_cast<float>(preset.vrc6Duty));
        }
        else
        {
            set(ids::n163Wave, static_cast<float>(preset.n163Wave));
            set(ids::n163Channels, static_cast<float>(preset.n163Channels));
        }
    }
    else if (preset.console == static_cast<int>(Console::Ym2612))
    {
        set(ids::genPatch, static_cast<float>(preset.genPatch));
        set(ids::genAlgorithm, static_cast<float>(preset.genAlgorithm));
        set(ids::genFeedback, static_cast<float>(preset.genFeedback));
        set(ids::genBrightness, preset.genBrightness);
        set(ids::genAttack, preset.genAttack);
        set(ids::genDecay, preset.genDecay);
        set(ids::genRelease, preset.genRelease);
    }
    else if (preset.console == static_cast<int>(Console::Psg))
    {
        set(ids::psgChannel, static_cast<float>(preset.psgChannel));
        set(ids::psgNoiseType, static_cast<float>(preset.psgNoiseType));
        set(ids::psgNoiseRate, static_cast<float>(preset.psgNoiseRate));
        set(ids::psgAttack, preset.psgAttack);
        set(ids::psgDecay, preset.psgDecay);
        set(ids::psgSustain, preset.psgSustain);
        set(ids::psgRelease, preset.psgRelease);
    }
    else
    {
        // The root note and the loop belong to the user's sample, so a preset leaves them as they are.
        set(ids::snesInstrument, static_cast<float>(preset.snesInstrument));
        set(ids::snesAttack, static_cast<float>(preset.snesAttack));
        set(ids::snesDecay, static_cast<float>(preset.snesDecay));
        set(ids::snesSustainLevel, static_cast<float>(preset.snesSustainLevel));
        set(ids::snesSustainRate, static_cast<float>(preset.snesSustainRate));
        set(ids::snesRelease, preset.snesRelease);
        set(ids::snesEchoMix, preset.snesEchoMix);
        set(ids::snesEchoDelay, static_cast<float>(preset.snesEchoDelay));
        set(ids::snesEchoFeedback, preset.snesEchoFeedback);
    }

    set(ids::vibratoRate, preset.vibratoRate);
    set(ids::vibratoDepth, preset.vibratoDepth);
    set(ids::glide, preset.glide);

    currentProgram.store(index);
    updateHostDisplay(ChangeDetails().withProgramChanged(true)); // the host's own preset menu follows
}

const juce::String RetroChipProcessor::getProgramName(int index)
{
    if (index < 0 || index >= getNumPrograms())
        return {};
    return kPresets[static_cast<size_t>(index)].name;
}

void RetroChipProcessor::getStateInformation(juce::MemoryBlock& destination)
{
    auto state = apvts.copyState();
    state.setProperty(kProgramProperty, currentProgram.load(), nullptr);
    {
        const juce::ScopedLock lock(sampleLock);
        if (userSample != nullptr)
        {
            state.setProperty(kSampleProperty, sampleToBase64(*userSample), nullptr);
            state.setProperty(kSampleNameProperty, juce::String::fromUTF8(userSample->name.c_str()), nullptr);
        }
        else
        {
            state.removeProperty(kSampleProperty, nullptr);
            state.removeProperty(kSampleNameProperty, nullptr);
        }

        if (userSound != nullptr && userSound->patch.has_value())
        {
            state.setProperty(kGenesisTfiProperty, bytesToBase64(userSound->tfi.data(), userSound->tfi.size()), nullptr);
            state.setProperty(kGenesisTfiNameProperty, juce::String::fromUTF8(userSound->patchName.c_str()), nullptr);
        }
        if (userSound != nullptr && !userSound->pcm.empty())
        {
            state.setProperty(kGenesisPcmProperty, bytesToBase64(userSound->pcm.data(), userSound->pcm.size()), nullptr);
            state.setProperty(kGenesisPcmNameProperty, juce::String::fromUTF8(userSound->pcmName.c_str()), nullptr);
        }
        if (userSound != nullptr && !userSound->kit.empty())
        {
            juce::ValueTree kit(kGenesisKitTree);
            for (const auto& sample : userSound->kit)
            {
                juce::ValueTree child(kGenesisKitSampleTree);
                child.setProperty(kGenesisKitNoteProperty, sample.note, nullptr);
                child.setProperty(kGenesisKitNameProperty, juce::String::fromUTF8(sample.name.c_str()), nullptr);
                child.setProperty(kGenesisKitDataProperty, bytesToBase64(sample.pcm.data(), sample.pcm.size()), nullptr);
                kit.appendChild(child, nullptr);
            }
            state.appendChild(kit, nullptr);
        }

        if (nesImports != nullptr)
        {
            for (std::size_t chip = 0; chip < nesImports->files.size(); ++chip)
                if (!nesImports->files[chip].empty())
                    state.setProperty(kNesFtiProperties[chip], bytesToBase64(nesImports->files[chip].data(), nesImports->files[chip].size()), nullptr);
            if (const auto& sample = nesImports->sound.dpcm)
            {
                state.setProperty(kNesDpcmProperty, bytesToBase64(sample->bytes.data(), sample->bytes.size()), nullptr);
                state.setProperty(kNesDpcmNameProperty, juce::String::fromUTF8(sample->name.c_str()), nullptr);
            }
        }
    }
    {
        const juce::ScopedLock lock(soundFontLock);
        const auto& choice = loading ? requestedChoice : currentChoice;
        if (choice.path.isNotEmpty())
        {
            state.setProperty(kSfPathProperty, choice.path, nullptr);
            state.setProperty(kSfBankProperty, choice.bank, nullptr);
            state.setProperty(kSfProgramProperty, choice.program, nullptr);
            state.setProperty(kSfNameProperty, choice.name, nullptr);
        }
    }
    if (const auto xml = state.createXml())
        copyXmlToBinary(*xml, destination);
}

void RetroChipProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(apvts.state.getType()))
        return;

    const auto state = juce::ValueTree::fromXml(*xml);
    apvts.replaceState(state);
    currentProgram.store(juce::jlimit(0, getNumPrograms() - 1, static_cast<int>(state.getProperty(kProgramProperty, 0))));
    updateHostDisplay(ChangeDetails().withProgramChanged(true));
    restoreSoundFont(state);
    restoreGenesisSound(state);
    restoreNes(state);

    // The sample is not a parameter: the state's own copy of it goes, and the engine gets the saved one. A
    // sample that cannot be read leaves the instrument silent, with a message, and the rest as loaded.
    const auto encoded = state.getProperty(kSampleProperty).toString();
    const auto name = state.getProperty(kSampleNameProperty).toString();
    apvts.state.removeProperty(kSampleProperty, nullptr);
    apvts.state.removeProperty(kSampleNameProperty, nullptr);
    if (encoded.isEmpty())
    {
        publishSample(nullptr, {});
        return;
    }
    auto sample = sampleFromBase64(encoded, name);
    juce::String message;
    if (sample != nullptr)
        sample->rootNote = juce::roundToInt(snesRootNote->load());
    else
        message = tr("Unreadable sample in the saved state: import it again");
    publishSample(std::move(sample), message);
}

juce::String RetroChipProcessor::importSample(const juce::File& file, bool* imported)
{
    auto result = importWav(file);
    if (imported != nullptr)
        *imported = result.sample != nullptr;
    if (result.sample != nullptr)
    {
        result.sample->rootNote = juce::roundToInt(snesRootNote->load());
        publishSample(std::move(result.sample), result.message);
        return result.message;
    }

    // The current sample stays; only the message changes.
    {
        const juce::ScopedLock lock(sampleLock);
        sampleMessage = result.message;
    }
    notifySampleChanged();
    return result.message;
}

// Hands a finished sample (or none) to the audio thread. Everything the engine will read is written by now.
void RetroChipProcessor::publishSample(std::unique_ptr<SnesSample> sample, const juce::String& message)
{
    {
        const juce::ScopedLock lock(sampleLock);
        freeRetiredSamples();

        // The audio thread may have taken the sample before this one and not yet stored its acknowledgement,
        // and the acknowledgement left from earlier (a null, say) could then match the new sample by chance.
        // Clearing it first leaves one way to match before the audio thread has taken what is published now:
        // a late store of a null, when this one is null too. The engine's pointer is already null by then,
        // so none of the retired samples is in use and freeing them is safe.
        adoptedSample.store(noNewSample());

        const auto* published = sample.get();
        const auto* previous = incomingSample.exchange(published);
        if (previous == noNewSample() && userSample != nullptr)
            retiredSamples.push_back(std::move(userSample)); // the audio thread has it, or may be taking it
        // Otherwise the audio thread never saw the one it replaces, and the assignment below frees it.

        userSample = std::move(sample);
        sampleMessage = message;
        freeRetiredSamples();
    }
    notifySampleChanged();
}

// With the lock held. The newest sample is the one the audio thread has switched to, so none of the older
// ones can be in use. Until then they stay, which is for as long as the audio thread does not run.
void RetroChipProcessor::freeRetiredSamples()
{
    if (!retiredSamples.empty() && adoptedSample.load() == userSample.get())
        retiredSamples.clear();
}

void RetroChipProcessor::releaseRetiredSamples()
{
    const juce::ScopedLock lock(sampleLock);
    freeRetiredSamples();
    freeRetiredSounds();
    freeRetiredNes();
}

int RetroChipProcessor::getRetiredSampleCount() const
{
    const juce::ScopedLock lock(sampleLock);
    return static_cast<int>(retiredSamples.size());
}

juce::String RetroChipProcessor::getUserSampleName() const
{
    const juce::ScopedLock lock(sampleLock);
    return userSample != nullptr ? juce::String::fromUTF8(userSample->name.c_str()) : juce::String();
}

juce::String RetroChipProcessor::getSampleStatus() const
{
    const juce::ScopedLock lock(sampleLock);
    if (sampleMessage.isNotEmpty())
        return sampleMessage;
    if (userSample != nullptr)
        return userSample->name.empty() ? tr("Imported sample") : juce::String::fromUTF8(userSample->name.c_str());

    // Read from the parameter itself: the editor asks from a change callback, which can run before the raw
    // value has caught up.
    const auto* instrument = apvts.getParameter(ids::snesInstrument);
    if (juce::roundToInt(instrument->convertFrom0to1(instrument->getValue())) == SnesEngine::kUserInstrument)
        return tr("Import a WAV file to play this sound");
    return {};
}

void RetroChipProcessor::notifySampleChanged()
{
    // The editor listens; from another thread the notification waits for the message thread.
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        sampleChanges.sendSynchronousChangeMessage();
    else
        sampleChanges.sendChangeMessage();
}

juce::String RetroChipProcessor::importGenesisFile(const juce::File& file, GenesisImport* imported)
{
    auto taken = GenesisImport::none;
    auto next = copyOfUserSound();

    juce::String message;
    if (file.hasFileExtension("tfi"))
    {
        auto result = importTfi(file);
        message = result.message;
        if (result.patch)
        {
            next->patch = result.patch;
            next->tfi = result.file;
            next->patchName = result.name;
            taken = GenesisImport::patch;
        }
    }
    else if (file.hasFileExtension("wav;wave"))
    {
        auto result = importPcm(file);
        message = result.message;
        if (!result.pcm.empty())
        {
            next->pcm = std::move(result.pcm);
            next->pcmName = result.name;
            taken = GenesisImport::pcm;
        }
    }
    else
    {
        message = tr("Unsupported format: ") + file.getFileName();
    }

    if (imported != nullptr)
        *imported = taken;

    if (taken != GenesisImport::none)
    {
        publishSound(std::move(next), message);
        return message;
    }

    // The current sound stays; only the message changes.
    {
        const juce::ScopedLock lock(sampleLock);
        genesisMessage = message;
    }
    notifySampleChanged();
    return message;
}

// The state's copy of these goes, as with the sample. What cannot be read leaves that sound silent, with a message.
void RetroChipProcessor::restoreGenesisSound(const juce::ValueTree& state)
{
    const auto tfi = state.getProperty(kGenesisTfiProperty).toString();
    const auto tfiName = state.getProperty(kGenesisTfiNameProperty).toString();
    const auto pcm = state.getProperty(kGenesisPcmProperty).toString();
    const auto pcmName = state.getProperty(kGenesisPcmNameProperty).toString();
    const auto kit = state.getChildWithName(kGenesisKitTree); // kept by this, whatever is removed below
    for (const auto* property : { kGenesisTfiProperty, kGenesisTfiNameProperty, kGenesisPcmProperty, kGenesisPcmNameProperty })
        apvts.state.removeProperty(property, nullptr);
    apvts.state.removeChild(apvts.state.getChildWithName(kGenesisKitTree), nullptr);

    auto sound = std::make_unique<GenesisUserSound>();
    if (tfi.isNotEmpty())
    {
        const auto bytes = bytesFromBase64(tfi, kTfiSize);
        sound->patch = parseTfi(bytes.data(), bytes.size());
        if (sound->patch)
        {
            std::copy(bytes.begin(), bytes.end(), sound->tfi.begin());
            sound->patchName = tfiName.toStdString();
        }
    }
    if (pcm.isNotEmpty())
    {
        sound->pcm = bytesFromBase64(pcm, static_cast<size_t>(kMaxPcmBytes));
        sound->pcmName = pcmName.toStdString();
    }

    // A sample that cannot be used costs its key and no others.
    bool damaged = (tfi.isNotEmpty() && !sound->patch) || (pcm.isNotEmpty() && sound->pcm.empty());
    auto bytesLeft = static_cast<size_t>(kMaxPcmBytes);
    for (const auto& child : kit)
    {
        const int note = child.getProperty(kGenesisKitNoteProperty, -1);
        const bool taken = std::any_of(sound->kit.begin(), sound->kit.end(), [note](const PcmKitSample& s) { return s.note == note; });
        auto bytes = bytesFromBase64(child.getProperty(kGenesisKitDataProperty).toString(), bytesLeft);
        if (note < kFirstKitNote || note >= kFirstKitNote + kMaxKitSamples || taken || bytes.empty())
        {
            damaged = true;
            continue;
        }
        bytesLeft -= bytes.size();
        sound->kit.push_back({ note, std::move(bytes), child.getProperty(kGenesisKitNameProperty).toString().toStdString() });
    }

    if (sound->pcm.empty())
        sound->pcmName.clear();
    const bool nothing = !sound->patch && sound->pcm.empty() && sound->kit.empty();
    publishSound(nothing ? nullptr : std::move(sound), damaged ? tr("Unreadable imported sound in the saved state: import it again") : juce::String());
}

// The rule of publishSample, whose comments say why it goes in this order.
void RetroChipProcessor::publishSound(std::unique_ptr<GenesisUserSound> sound, const juce::String& message)
{
    {
        const juce::ScopedLock lock(sampleLock);
        freeRetiredSounds();
        adoptedSound.store(noNewSound());

        const auto* published = sound.get();
        const auto* previous = incomingSound.exchange(published);
        if (previous == noNewSound() && userSound != nullptr)
            retiredSounds.push_back(std::move(userSound));

        userSound = std::move(sound);
        genesisMessage = message;
        freeRetiredSounds();
    }
    notifySampleChanged();
}

// With the lock held. The rule of freeRetiredSamples.
void RetroChipProcessor::freeRetiredSounds()
{
    if (!retiredSounds.empty() && adoptedSound.load() == userSound.get())
        retiredSounds.clear();
}

int RetroChipProcessor::getRetiredSoundCount() const
{
    const juce::ScopedLock lock(sampleLock);
    return static_cast<int>(retiredSounds.size());
}

juce::String RetroChipProcessor::importNesFile(const juce::File& file, NesImport* imported, int* chip)
{
    auto taken = NesImport::none;
    auto next = copyOfNesImports();
    juce::String message;

    if (file.hasFileExtension("fti"))
    {
        auto result = importFti(file);
        message = result.message;
        if (result.instrument)
        {
            const int index = result.instrument->chip == FtiChip::Vrc6 ? 1 : result.instrument->chip == FtiChip::N163 ? 2 : 0;
            next->sound.instruments[static_cast<std::size_t>(index)] = std::move(result.instrument);
            next->files[static_cast<std::size_t>(index)] = std::move(result.file);
            taken = NesImport::instrument;
            if (chip != nullptr)
                *chip = index;
        }
    }
    else if (file.hasFileExtension("wav;wave"))
    {
        auto result = importDpcm(file);
        message = result.message;
        if (!result.bytes.empty())
        {
            next->sound.dpcm = DpcmSample { std::move(result.bytes), std::move(result.name) };
            taken = NesImport::dpcm;
        }
    }
    else
    {
        message = tr("Unsupported format: ") + file.getFileName();
    }

    if (imported != nullptr)
        *imported = taken;
    if (taken != NesImport::none)
    {
        publishNes(std::move(next), message);
        return message;
    }

    {
        const juce::ScopedLock lock(sampleLock);
        nesMessage = message;
    }
    notifySampleChanged();
    return message;
}

std::unique_ptr<NesImports> RetroChipProcessor::copyOfNesImports() const
{
    auto copy = std::make_unique<NesImports>();
    const juce::ScopedLock lock(sampleLock);
    if (nesImports != nullptr)
        *copy = *nesImports;
    return copy;
}

// The state's copy of these goes, as with the sample. What cannot be read is left out, with a message.
void RetroChipProcessor::restoreNes(const juce::ValueTree& state)
{
    auto imports = std::make_unique<NesImports>();
    bool damaged = false;
    for (std::size_t chip = 0; chip < imports->files.size(); ++chip)
    {
        const auto encoded = state.getProperty(kNesFtiProperties[chip]).toString();
        apvts.state.removeProperty(kNesFtiProperties[chip], nullptr);
        if (encoded.isEmpty())
            continue;
        auto bytes = bytesFromBase64(encoded, static_cast<size_t>(kMaxFtiBytes));
        auto instrument = ftiFromBytes(bytes);
        const auto expected = chip == 1 ? FtiChip::Vrc6 : chip == 2 ? FtiChip::N163 : FtiChip::Ricoh2A03;
        if (!instrument || instrument->chip != expected)
        {
            damaged = true;
            continue;
        }
        imports->sound.instruments[chip] = std::move(instrument);
        imports->files[chip] = std::move(bytes);
    }

    const auto dpcmData = state.getProperty(kNesDpcmProperty).toString();
    const auto dpcmName = state.getProperty(kNesDpcmNameProperty).toString();
    apvts.state.removeProperty(kNesDpcmProperty, nullptr);
    apvts.state.removeProperty(kNesDpcmNameProperty, nullptr);
    if (dpcmData.isNotEmpty())
    {
        auto bytes = bytesFromBase64(dpcmData, static_cast<size_t>(dpcm::kMaxBytes));
        if (bytes.empty())
            damaged = true;
        else
            imports->sound.dpcm = DpcmSample { std::move(bytes), dpcmName.toStdString() };
    }

    const bool nothing = !imports->sound.dpcm && std::none_of(imports->sound.instruments.begin(), imports->sound.instruments.end(),
                                                               [](const auto& instrument) { return instrument.has_value(); });
    publishNes(nothing ? nullptr : std::move(imports), damaged ? tr("Unreadable imported sound in the saved state: import it again") : juce::String());
}

// The rule of publishSample.
void RetroChipProcessor::publishNes(std::unique_ptr<NesImports> imports, const juce::String& message)
{
    {
        const juce::ScopedLock lock(sampleLock);
        freeRetiredNes();
        adoptedNes.store(noNewNes());

        const auto* published = imports.get();
        const auto* previous = incomingNes.exchange(published);
        if (previous == noNewNes() && nesImports != nullptr)
            retiredNes.push_back(std::move(nesImports));

        nesImports = std::move(imports);
        nesMessage = message;
        freeRetiredNes();
    }
    notifySampleChanged();
}

// With the lock held. The rule of freeRetiredSamples.
void RetroChipProcessor::freeRetiredNes()
{
    if (!retiredNes.empty() && adoptedNes.load() == nesImports.get())
        retiredNes.clear();
}

int RetroChipProcessor::getRetiredNesSoundCount() const
{
    const juce::ScopedLock lock(sampleLock);
    return static_cast<int>(retiredNes.size());
}

juce::String RetroChipProcessor::getNesInstrumentName(int chip) const
{
    const juce::ScopedLock lock(sampleLock);
    if (nesImports == nullptr || chip < 0 || chip > 2)
        return {};
    const auto& instrument = nesImports->sound.instruments[static_cast<std::size_t>(chip)];
    return instrument ? juce::String::fromUTF8(instrument->name.c_str()) : juce::String();
}

bool RetroChipProcessor::hasNesInstrument(int chip) const
{
    const juce::ScopedLock lock(sampleLock);
    return nesImports != nullptr && chip >= 0 && chip <= 2 && nesImports->sound.instruments[static_cast<std::size_t>(chip)].has_value();
}

bool RetroChipProcessor::isNesKitOnly() const
{
    const juce::ScopedLock lock(sampleLock);
    if (nesImports == nullptr || !nesImports->sound.instruments[0])
        return false;
    const auto& instrument = *nesImports->sound.instruments[0];
    return !instrument.keys.empty() && !instrument.sequences[kFtiVolume].enabled;
}

juce::String RetroChipProcessor::getDpcmName() const
{
    const juce::ScopedLock lock(sampleLock);
    return nesImports != nullptr && nesImports->sound.dpcm ? juce::String::fromUTF8(nesImports->sound.dpcm->name.c_str()) : juce::String();
}

juce::String RetroChipProcessor::getNesStatus() const
{
    const juce::ScopedLock lock(sampleLock);
    if (nesMessage.isNotEmpty())
        return nesMessage;

    // Read from the parameters themselves, as getSampleStatus() does.
    const auto valueOf = [this](const char* id)
    {
        const auto* parameter = apvts.getParameter(id);
        return juce::roundToInt(parameter->convertFrom0to1(parameter->getValue()));
    };
    const int chip = nesChipOf(static_cast<Console>(juce::jlimit(0, static_cast<int>(Console::N163), valueOf(ids::console))));
    if (chip < 0)
        return {};

    const auto* sound = nesImports != nullptr ? &nesImports->sound : nullptr;
    if (chip == 0 && valueOf(ids::channel) == static_cast<int>(NesChannel::Dpcm))
    {
        const bool kit = valueOf(ids::nesUseInstrument) != 0 && sound != nullptr && sound->instruments[0] && !sound->instruments[0]->keys.empty();
        if (kit)
        {
            const auto count = static_cast<int>(sound->instruments[0]->keys.size());
            return tr("Kit: ") + juce::String(count) + (count > 1 ? tr(" sounds") : tr(" sound"));
        }
        if (sound != nullptr && sound->dpcm)
            return juce::String::fromUTF8(sound->dpcm->name.c_str());
        return tr("Import a WAV or an .fti with samples");
    }

    const auto& instrument = sound != nullptr ? sound->instruments[static_cast<std::size_t>(chip)] : std::nullopt;
    return instrument ? juce::String::fromUTF8(instrument->name.c_str()) : tr("Import an .fti file for a game instrument");
}

juce::String RetroChipProcessor::getNesKitDescription() const
{
    const juce::ScopedLock lock(sampleLock);
    juce::StringArray lines;
    if (nesImports != nullptr && nesImports->sound.instruments[0])
    {
        const auto& instrument = *nesImports->sound.instruments[0];
        for (const auto& key : instrument.keys)
            lines.add(juce::MidiMessage::getMidiNoteName(key.note, true, true, 3) + tr(": ")
                      + juce::String::fromUTF8(instrument.samples[static_cast<std::size_t>(key.sample)].name.c_str()));
    }
    return lines.joinIntoString("\n");
}

juce::String RetroChipProcessor::importGenesisKit(const juce::Array<juce::File>& files, bool* imported)
{
    auto result = importKit(files);
    if (imported != nullptr)
        *imported = !result.samples.empty();

    if (!result.samples.empty())
    {
        auto next = copyOfUserSound();
        next->kit = std::move(result.samples);
        publishSound(std::move(next), result.message);
        return result.message;
    }

    // The current kit stays; only the message changes.
    {
        const juce::ScopedLock lock(sampleLock);
        genesisMessage = result.message;
    }
    notifySampleChanged();
    return result.message;
}

juce::String RetroChipProcessor::getKitDescription() const
{
    const juce::ScopedLock lock(sampleLock);
    juce::StringArray lines;
    if (userSound != nullptr)
        for (const auto& sample : userSound->kit)
            lines.add(juce::MidiMessage::getMidiNoteName(sample.note, true, true, 3) + tr(": ") + juce::String::fromUTF8(sample.name.c_str()));
    return lines.joinIntoString("\n");
}

std::unique_ptr<GenesisUserSound> RetroChipProcessor::copyOfUserSound() const
{
    auto copy = std::make_unique<GenesisUserSound>();
    const juce::ScopedLock lock(sampleLock);
    if (userSound != nullptr)
        *copy = *userSound;
    return copy;
}

juce::String RetroChipProcessor::getUserPatchName() const
{
    const juce::ScopedLock lock(sampleLock);
    return userSound != nullptr && userSound->patch.has_value() ? juce::String::fromUTF8(userSound->patchName.c_str()) : juce::String();
}

juce::String RetroChipProcessor::getUserPcmName() const
{
    const juce::ScopedLock lock(sampleLock);
    return userSound != nullptr ? juce::String::fromUTF8(userSound->pcmName.c_str()) : juce::String();
}

std::optional<FmPatch> RetroChipProcessor::getUserPatch() const
{
    const juce::ScopedLock lock(sampleLock);
    return userSound != nullptr ? userSound->patch : std::nullopt;
}

juce::String RetroChipProcessor::getGenesisStatus() const
{
    const juce::ScopedLock lock(sampleLock);
    if (genesisMessage.isNotEmpty())
        return genesisMessage;

    // Read from the parameter itself, as getSampleStatus() does.
    const auto* patch = apvts.getParameter(ids::genPatch);
    const int timbre = juce::roundToInt(patch->convertFrom0to1(patch->getValue()));
    if (timbre == GenesisEngine::kUserPatch)
        return userSound != nullptr && userSound->patch.has_value() ? juce::String::fromUTF8(userSound->patchName.c_str())
                                                                    : tr("Import a .tfi file to play this timbre");
    if (timbre == GenesisEngine::kPcmPatch)
        return userSound != nullptr && !userSound->pcm.empty() ? juce::String::fromUTF8(userSound->pcmName.c_str())
                                                               : tr("Import a WAV file to play this sound");
    if (timbre == GenesisEngine::kPcmKitPatch)
    {
        if (userSound == nullptr || userSound->kit.empty())
            return tr("Import several WAV files to play this kit");
        return tr("Kit: ") + juce::String(static_cast<int>(userSound->kit.size())) + (userSound->kit.size() > 1 ? tr(" sounds") : tr(" sound"));
    }
    return {};
}

void RetroChipProcessor::setLibraryRoot(const juce::File& root)
{
    const juce::ScopedLock lock(soundFontLock);
    libraryRoot = root;
}

juce::File RetroChipProcessor::getLibraryRoot() const
{
    const juce::ScopedLock lock(soundFontLock);
    return libraryRoot;
}

void RetroChipProcessor::setLibraryRoot(Library library, const juce::File& root)
{
    if (library == Library::soundFonts)
        return setLibraryRoot(root);
    const juce::ScopedLock lock(soundFontLock);
    (library == Library::tfi ? tfiRoot : ftiRoot) = root;
}

juce::File RetroChipProcessor::getLibraryRoot(Library library) const
{
    if (library == Library::soundFonts)
        return getLibraryRoot();
    const juce::ScopedLock lock(soundFontLock);
    return library == Library::tfi ? tfiRoot : ftiRoot;
}

void RetroChipProcessor::selectSoundFont(const juce::String& relativePath, int bank, int program)
{
    if (relativePath.isEmpty())
        clearSoundFont();
    else
        requestSoundFont({ relativePath, bank, program, {} }, false);
}

// A project that opened with its file missing, or unreadable with nothing playing, holds the choice without a bank.
// The same request is made again as the project would have: from its state, so the bank and program are kept.
void RetroChipProcessor::retrySoundFont()
{
    const juce::ScopedLock lock(soundFontLock);
    if (loading || activeBank != nullptr || currentChoice.path.isEmpty())
        return;

    requestSoundFont(currentChoice, true);
}

// The state's copy of these goes, as with the sample: they are not parameters, and they would come back in the
// next save even with another file in use. A state with no file means no SoundFont, without a message.
void RetroChipProcessor::restoreSoundFont(const juce::ValueTree& state)
{
    SoundFontChoice choice;
    choice.path = state.getProperty(kSfPathProperty).toString();
    choice.bank = state.getProperty(kSfBankProperty, -1);
    choice.program = state.getProperty(kSfProgramProperty, -1);
    choice.name = state.getProperty(kSfNameProperty).toString();
    for (const auto* property : { kSfPathProperty, kSfBankProperty, kSfProgramProperty, kSfNameProperty })
        apvts.state.removeProperty(property, nullptr);

    if (choice.path.isEmpty())
        clearSoundFont();
    else
        requestSoundFont(choice, true);
}

void RetroChipProcessor::requestSoundFont(const SoundFontChoice& choice, bool fromState)
{
    const juce::ScopedLock lock(soundFontLock);
    soundFontRevision.fetch_add(1);

    const auto file = resolveInLibrary(libraryRoot, choice.path);
    if (file == juce::File())
    {
        soundFontLoader.cancel();
        loading = false;
        applyLoadFailure(SoundFontLoader::Failure::missing, choice, fromState,
                         choice.path.replaceCharacter('\\', '/').fromLastOccurrenceOf("/", false, false));
        return;
    }

    soundFontMessage.clear();
    requestedChoice = choice;
    requestedFromState = fromState;
    loading = true;
    loadId = soundFontLoader.request(file, knownSampleRate.load(), choice.bank, choice.program);
}

void RetroChipProcessor::clearSoundFont()
{
    const juce::ScopedLock lock(soundFontLock);
    soundFontLoader.cancel();
    loading = false;
    soundFontMessage.clear();
    soundFontInstruments.clear();
    currentChoice = {};
    soundFontInstrument.store(0);
    publishBank(nullptr);
    soundFontRevision.fetch_add(1);
}

// On the loader's thread. The message thread takes it from here, which needs nothing but the message loop running
// and is cancelled in the destructor.
void RetroChipProcessor::receiveLoad(SoundFontLoader::Result result)
{
    {
        auto finished = std::make_unique<SoundFontLoader::Result>(std::move(result));
        const juce::ScopedLock lock(soundFontLock);
        finishedLoad.swap(finished);
        // An older one no one took, now in `finished`, was never published and goes here.
    }
    triggerAsyncUpdate();
}

void RetroChipProcessor::takeFinishedLoad()
{
    std::unique_ptr<SoundFontLoader::Result> result; // declared first, so a dropped bank is freed outside the lock
    const juce::ScopedLock lock(soundFontLock);
    result = std::move(finishedLoad);
    if (result == nullptr || !loading || result->id != loadId)
        return;

    loading = false;
    soundFontRevision.fetch_add(1);

    if (result->failure != SoundFontLoader::Failure::none)
    {
        applyLoadFailure(result->failure, requestedChoice, requestedFromState, result->file.getFileName());
        return;
    }

    currentChoice = requestedChoice;
    soundFontInstruments = std::move(result->instruments);
    soundFontMessage.clear();
    const auto& instrument = soundFontInstruments[static_cast<size_t>(result->instrument)];
    if (result->instrumentFound || currentChoice.bank < 0)
    {
        currentChoice.bank = instrument.bank;
        currentChoice.program = instrument.program;
        currentChoice.name = instrument.name;
    }
    else
    {
        // The project goes on naming the instrument it was saved with, until a choice is made here.
        soundFontMessage = currentChoice.name.isNotEmpty()
                               ? tr("Instrument not found (") + currentChoice.name + tr("): first instrument used")
                               : tr("Instrument not found: first instrument used");
    }

    soundFontInstrument.store(result->instrument);
    publishBank(std::move(result->bank));
}

// With the lock held. A file that fails while being reopened with a project is missing or is not usable. Missing
// means silence. Otherwise what plays stays, as it does for any failed choice made here. A project's file is kept
// for the next save whenever nothing else is playing in its place, so the project still has it when the file is back.
void RetroChipProcessor::applyLoadFailure(SoundFontLoader::Failure failure, const SoundFontChoice& choice, bool fromState,
                                          const juce::String& fileName)
{
    soundFontMessage = loadFailureText(failure, fileName);
    if (!fromState)
        return;

    if (failure == SoundFontLoader::Failure::missing)
    {
        soundFontInstruments.clear();
        soundFontInstrument.store(0);
        publishBank(nullptr);
        currentChoice = choice;
    }
    else if (activeBank == nullptr)
    {
        currentChoice = choice;
    }
}

// With the lock held. Hands a finished bank (or none) to the audio thread; see publishSample for why in this order.
void RetroChipProcessor::publishBank(std::unique_ptr<SoundFontBank> bank)
{
    freeRetiredBanks();
    adoptedBank.store(noNewBank());

    auto* published = bank.get();
    const auto* previous = incomingBank.exchange(published);
    if (previous == noNewBank() && activeBank != nullptr)
        retiredBanks.push_back(std::move(activeBank)); // the audio thread has it, or may be taking it
    // Otherwise the audio thread never saw the one it replaces, and the assignment below frees it.

    activeBank = std::move(bank);
    freeRetiredBanks();
    if (!retiredBanks.empty())
        startTimer(kRetiredBankPollMs);
}

// With the lock held. The rule of freeRetiredSamples.
void RetroChipProcessor::freeRetiredBanks()
{
    if (!retiredBanks.empty() && adoptedBank.load() == activeBank.get())
        retiredBanks.clear();
}

void RetroChipProcessor::releaseRetiredBanks()
{
    const juce::ScopedLock lock(soundFontLock);
    freeRetiredBanks();
}

int RetroChipProcessor::getRetiredBankCount() const
{
    const juce::ScopedLock lock(soundFontLock);
    return static_cast<int>(retiredBanks.size());
}

void RetroChipProcessor::timerCallback()
{
    const juce::ScopedLock lock(soundFontLock);
    freeRetiredBanks();
    if (retiredBanks.empty())
        stopTimer();
}

juce::String RetroChipProcessor::getSoundFontPath() const
{
    const juce::ScopedLock lock(soundFontLock);
    return loading ? requestedChoice.path : currentChoice.path;
}

std::vector<SoundFontInstrument> RetroChipProcessor::getSoundFontInstruments() const
{
    const juce::ScopedLock lock(soundFontLock);
    return soundFontInstruments;
}

int RetroChipProcessor::getSoundFontInstrumentIndex() const
{
    return soundFontInstrument.load();
}

void RetroChipProcessor::selectSoundFontInstrument(int index)
{
    const juce::ScopedLock lock(soundFontLock);
    if (index < 0 || index >= static_cast<int>(soundFontInstruments.size()))
        return;

    const auto& instrument = soundFontInstruments[static_cast<size_t>(index)];
    currentChoice.bank = instrument.bank;
    currentChoice.program = instrument.program;
    currentChoice.name = instrument.name;
    soundFontMessage.clear();
    soundFontInstrument.store(index);
    soundFontRevision.fetch_add(1);
}

juce::String RetroChipProcessor::getSoundFontStatus() const
{
    const juce::ScopedLock lock(soundFontLock);
    return soundFontMessage;
}

bool RetroChipProcessor::isSoundFontLoading() const
{
    const juce::ScopedLock lock(soundFontLock);
    return loading;
}

int RetroChipProcessor::getSoundFontRevision() const
{
    return soundFontRevision.load();
}
} // namespace retrochip

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new retrochip::RetroChipProcessor();
}
