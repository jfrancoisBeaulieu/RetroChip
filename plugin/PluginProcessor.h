// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include "ConsoleEngine.h"
#include "NesEngine.h"
#include "genesis/GenesisEngine.h"
#include "genesis/PsgEngine.h"
#include "snes/SnesEngine.h"
#include "soundfont/SoundFontBank.h"
#include "soundfont/SoundFontEngine.h"
#include "Parameters.h"
#include "SoundFontLibrary.h"
#include "SoundFontLoader.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace retrochip
{
// What importNesFile() took from a file.
enum class NesImport
{
    none,
    instrument,
    dpcm
};

// The NES's imports: what the engine plays, and the files the instruments came from, for the state.
struct NesImports
{
    NesUserSound sound;
    std::array<std::vector<std::uint8_t>, 3> files; // by NesChip
};

// What importGenesisFile() took from a file.
enum class GenesisImport
{
    none,
    patch,
    pcm
};

class RetroChipProcessor final : public juce::AudioProcessor,
                                 private juce::AudioProcessorValueTreeState::Listener,
                                 private juce::AsyncUpdater,
                                 private juce::Timer
{
public:
    RetroChipProcessor();
    ~RetroChipProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "RetroChip"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 6.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram.load(); }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destination) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // The console the user has selected. The audio thread switches to it at the start of the next block.
    Console getActiveConsole() const;

    // The engines' own state belongs to the audio thread, so tests may read it only while audio is stopped.
    ConsoleEngine* engineFor(Console console);

    // Imports a WAV file as the S-DSP's own sample, from the message thread; the file is read, converted and
    // compressed here, never on the audio thread. The sample becomes the instrument's at the start of the next
    // block. If the file cannot be used, the current sample stays. Returns what the status says about the file
    // (why it failed, or that it was cut), empty after a clean import. `imported`, if given, says whether a
    // sample was taken, which a cut one is; this does not touch the instrument parameter.
    juce::String importSample(const juce::File& file, bool* imported = nullptr);

    // The name of the user's sample, empty when there is none.
    juce::String getUserSampleName() const;

    // What the interface says about the sample: the last message, else its name, else, when the instrument
    // is the user's sample, an invitation to import. Empty if none of these applies.
    juce::String getSampleStatus() const;

    // Called after the sample or the status changes.
    juce::ChangeBroadcaster sampleChanges;

    // Replaced samples wait here until the audio thread has moved on to the newer one. This frees those that
    // can go; the message thread calls it now and then (an import or a state load also does).
    void releaseRetiredSamples();
    int getRetiredSampleCount() const;

    // Imports a .tfi file as the YM2612's own timbre, or a WAV file as its PCM sample, from the message thread,
    // under the same rules as importSample(). Each import keeps the other's sound. `imported` says what was taken.
    juce::String importGenesisFile(const juce::File& file, GenesisImport* imported = nullptr);

    // Imports WAV files as the YM2612's kit, in place of the one before. `imported` says whether there is a new one.
    juce::String importGenesisKit(const juce::Array<juce::File>& files, bool* imported = nullptr);

    // Which key plays what in the kit, one line for each, with the keys named as the keyboard shows them. Empty
    // when there is no kit.
    juce::String getKitDescription() const;

    // The names of the imports, empty when there are none, and the timbre itself.
    juce::String getUserPatchName() const;
    juce::String getUserPcmName() const;
    std::optional<FmPatch> getUserPatch() const;

    // What the interface says about them, in the manner of getSampleStatus(). Changes go through sampleChanges.
    juce::String getGenesisStatus() const;

    // Replaced sounds wait as replaced samples do, and releaseRetiredSamples() frees them too.
    int getRetiredSoundCount() const;

    // Imports an .fti file as its chip's instrument, or a WAV file as the delta-modulation channel's sample, from the
    // message thread, under the same rules as importSample(). Each import keeps the others. `chip` is the instrument's,
    // as a NesChip index.
    juce::String importNesFile(const juce::File& file, NesImport* imported = nullptr, int* chip = nullptr);

    // The imports' names, empty when there are none. A kit is a 2A03 instrument with samples and no volume sequence.
    juce::String getNesInstrumentName(int chip) const;
    bool hasNesInstrument(int chip) const;
    bool isNesKitOnly() const;
    juce::String getDpcmName() const;

    // What the interface says about them, in the manner of getSampleStatus(), and which key plays what in a kit.
    juce::String getNesStatus() const;
    juce::String getNesKitDescription() const;

    // Replaced imports wait as replaced samples do, and releaseRetiredSamples() frees them too.
    int getRetiredNesSoundCount() const;

    // The SoundFont console. All of these are for the message thread, and the host may also call them from
    // another non-audio thread. What they read is a copy kept for that purpose: the bank itself belongs to the
    // audio thread once it is published, and no pointer to it leaves here.
    //
    // The folder relative paths start from. It is not part of the saved state.
    void setLibraryRoot(const juce::File& root);
    juce::File getLibraryRoot() const;

    // The same for the folders of .tfi timbres and .fti instruments, whose files are imported as any other.
    void setLibraryRoot(Library library, const juce::File& root);
    juce::File getLibraryRoot(Library library) const;

    // Asks for a file of the library. It is read on the loader's thread, and arrives through the message loop:
    // from then on the next audio block plays it, and notes that sounded stop. If it cannot be used, the current
    // sound stays and the status says why. The bank and program are the instrument to start on, which the status
    // mentions if the file does not have it; leave them out for the file's first instrument. An empty path is no
    // SoundFont at all. The last file asked for wins.
    void selectSoundFont(const juce::String& relativePath, int bank = -1, int program = -1);

    // Asks again for the choice a project named, when nothing could be done with it: no file is loading, no
    // bank is in use, and a path is held (the file was missing or unreadable when the project opened). The
    // saved bank and program are kept, so the instrument comes back with the file. Anything else is ignored.
    void retrySoundFont();

    // The file a saved project would reopen: the last one asked for while it loads, then the one in use.
    juce::String getSoundFontPath() const;

    // The instruments of the file in use, in the bank's order, and the position of the one playing.
    std::vector<SoundFontInstrument> getSoundFontInstruments() const;
    int getSoundFontInstrumentIndex() const;

    // A position in that list. Anything else is ignored. Stops the notes, as any change of instrument does.
    void selectSoundFontInstrument(int index);

    // Why the last request did not do all it asked, in the words of the interface. Empty otherwise.
    juce::String getSoundFontStatus() const;

    // Whether a file asked for has yet to arrive.
    bool isSoundFontLoading() const;

    // Goes up each time one of the above changes, so a timer can tell when to read them again.
    int getSoundFontRevision() const;

    // Replaced banks wait here until the audio thread has moved on to the newer one. A timer of the processor's
    // frees those that can go, and so does each new file.
    void releaseRetiredBanks();
    int getRetiredBankCount() const;

    juce::AudioProcessorValueTreeState apvts;
    juce::MidiKeyboardState keyboardState;

private:
    NesParams readParams() const;
    void adoptPublishedNes();
    void publishNes(std::unique_ptr<NesImports> imports, const juce::String& message);
    void freeRetiredNes();
    void restoreNes(const juce::ValueTree& state);
    std::unique_ptr<NesImports> copyOfNesImports() const;
    GenesisParams readGenesisParams() const;
    SnesParams readSnesParams() const;
    SoundFontParams readSoundFontParams() const;
    PsgParams readPsgParams() const;
    int latencyFor(Console console) const;
    void handleMidi(ConsoleEngine& engine, const juce::MidiMessage& message);
    void adoptPublishedSample();
    void publishSample(std::unique_ptr<SnesSample> sample, const juce::String& message);
    void freeRetiredSamples();
    void notifySampleChanged();
    void adoptPublishedSound();
    void publishSound(std::unique_ptr<GenesisUserSound> sound, const juce::String& message);
    void freeRetiredSounds();
    void restoreGenesisSound(const juce::ValueTree& state);
    std::unique_ptr<GenesisUserSound> copyOfUserSound() const;

    // What a saved state names: a file of the library and an instrument, by the numbers that survive the file
    // gaining instruments. A bank of -1 is "its first", for a file not loaded yet.
    struct SoundFontChoice
    {
        juce::String path;
        int bank = -1;
        int program = -1;
        juce::String name;
    };

    void adoptPublishedBank();
    void requestSoundFont(const SoundFontChoice& choice, bool fromState);
    void clearSoundFont();
    void restoreSoundFont(const juce::ValueTree& state);
    void receiveLoad(SoundFontLoader::Result result);
    void takeFinishedLoad();
    void applyLoadFailure(SoundFontLoader::Failure failure, const SoundFontChoice& choice, bool fromState, const juce::String& fileName);
    void publishBank(std::unique_ptr<SoundFontBank> bank);
    void freeRetiredBanks();
    void timerCallback() override;

    // The latency is reported to the host from the message thread, never from the audio thread.
    void parameterChanged(const juce::String& parameterId, float newValue) override;
    void handleAsyncUpdate() override;

    NesEngine nesEngine;
    std::unique_ptr<GenesisEngine> genesisEngine; // large, and it allocates when built
    std::unique_ptr<SnesEngine> snesEngine;       // the same; the bank it plays is shared, see sharedSnesBank()
    std::unique_ptr<SoundFontEngine> soundFontEngine;
    std::unique_ptr<PsgEngine> psgEngine;         // large too, because of its resampler
    Console renderConsole = Console::Chip2A03; // audio thread only
    double heldBend = 0.0;                     // audio thread only: the wheel and the pedal as last played,
    bool pedalDown = false;                    // for the console being entered
    float previousGain = 1.0f;
    std::atomic<int> currentProgram { 0 };

    std::atomic<float>* channel;
    std::atomic<float>* duty;
    std::atomic<float>* noiseMode;
    std::atomic<float>* attack;
    std::atomic<float>* decay;
    std::atomic<float>* sustain;
    std::atomic<float>* release;
    std::atomic<float>* vibratoRate;
    std::atomic<float>* vibratoDepth;
    std::atomic<float>* glide;
    std::atomic<float>* velocity;
    std::atomic<float>* gain;
    std::atomic<float>* console;
    std::atomic<float>* genPatch;
    std::atomic<float>* genAlgorithm;
    std::atomic<float>* genFeedback;
    std::atomic<float>* genBrightness;
    std::atomic<float>* genAttack;
    std::atomic<float>* genDecay;
    std::atomic<float>* genRelease;
    std::atomic<float>* genRootNote;
    std::atomic<float>* genLoop;
    std::atomic<float>* snesInstrument;
    std::atomic<float>* snesAttack;
    std::atomic<float>* snesDecay;
    std::atomic<float>* snesSustainLevel;
    std::atomic<float>* snesSustainRate;
    std::atomic<float>* snesRelease;
    std::atomic<float>* snesEchoMix;
    std::atomic<float>* snesEchoDelay;
    std::atomic<float>* snesEchoFeedback;
    std::atomic<float>* snesRootNote;
    std::atomic<float>* snesLoop;
    std::atomic<float>* sfEchoMix;
    std::atomic<float>* sfEchoDelay;
    std::atomic<float>* sfEchoFeedback;
    std::atomic<float>* psgChannel;
    std::atomic<float>* psgNoiseType;
    std::atomic<float>* psgNoiseRate;
    std::atomic<float>* psgAttack;
    std::atomic<float>* psgDecay;
    std::atomic<float>* psgSustain;
    std::atomic<float>* psgRelease;
    std::atomic<float>* nesUseInstrument;
    std::atomic<float>* vrc6Channel;
    std::atomic<float>* vrc6Duty;
    std::atomic<float>* n163Wave;
    std::atomic<float>* n163Channels;
    std::atomic<float>* dpcmRootNote;
    std::atomic<float>* dpcmLoop;

    // The user's sample. The message thread builds it and owns every sample here; the audio thread only gets
    // a pointer, through `incomingSample`, and says which one it has switched to through `adoptedSample`.
    //  - incomingSample holds a sample the audio thread has not taken yet, or noNewSample(). Both threads swap
    //    it out with exchange(), so a sample is taken by exactly one of them: when the message thread gets one
    //    back, the audio thread never saw it and it is freed at once, which is what keeps imports with no
    //    audio running from piling up.
    //  - adoptedSample is the sample the engine has been given, set after setUserSample() returns. The message
    //    thread resets it to noNewSample() just before each publication. A replaced sample may be freed only
    //    once adoptedSample is the newest one: after that the audio thread cannot reach an older one. A store
    //    from an earlier adoption can land after the reset, but it matches the newest sample only when both
    //    are null, and then the engine's pointer is already null, so that is just as safe.
    // The lock is for the non-audio threads, which the host may mix (state loads, the editor); the audio
    // thread never takes it.
    mutable juce::CriticalSection sampleLock;
    std::unique_ptr<SnesSample> userSample;
    std::vector<std::unique_ptr<SnesSample>> retiredSamples;
    juce::String sampleMessage;
    std::atomic<const SnesSample*> incomingSample;
    std::atomic<const SnesSample*> adoptedSample;

    // The YM2612's imports, handed over by the rule written out for the sample above, with incomingSound,
    // adoptedSound and noNewSound() in the same parts, and under the same lock. A sound is never changed once
    // published: each import makes a new one, which is why both imports travel together.
    std::unique_ptr<GenesisUserSound> userSound;
    std::vector<std::unique_ptr<GenesisUserSound>> retiredSounds;
    juce::String genesisMessage;
    std::atomic<const GenesisUserSound*> incomingSound;
    std::atomic<const GenesisUserSound*> adoptedSound;

    // The NES's imports, by the same rule, with incomingNes, adoptedNes and noNewNes(), under the same lock.
    std::unique_ptr<NesImports> nesImports;
    std::vector<std::unique_ptr<NesImports>> retiredNes;
    juce::String nesMessage;
    std::atomic<const NesImports*> incomingNes;
    std::atomic<const NesImports*> adoptedNes;

    // The SoundFont bank, handed over by the rule written out for the sample above, with incomingBank,
    // adoptedBank and noNewBank() in the same parts. Each instance has its own bank, since a bank has one set of
    // voices. The bank goes to the audio thread alone: the message thread keeps only what it read before.
    // The instrument is the one thing the audio thread reads besides, and it is stored before the bank that goes
    // with it is published.
    mutable juce::CriticalSection soundFontLock;
    juce::File libraryRoot;
    juce::File tfiRoot { defaultLibraryRoot(Library::tfi) }; // under soundFontLock too
    juce::File ftiRoot { defaultLibraryRoot(Library::fti) };
    std::unique_ptr<SoundFontBank> activeBank;
    std::vector<std::unique_ptr<SoundFontBank>> retiredBanks;
    std::atomic<SoundFontBank*> incomingBank;
    std::atomic<SoundFontBank*> adoptedBank;
    std::atomic<int> soundFontInstrument { 0 };
    std::atomic<double> knownSampleRate { 44100.0 }; // the host's last word; a bank built at another follows on adoption
    std::vector<SoundFontInstrument> soundFontInstruments;
    SoundFontChoice currentChoice;   // what plays, or what a project named when nothing could
    SoundFontChoice requestedChoice; // while loading
    bool requestedFromState = false;
    bool loading = false;
    juce::uint64 loadId = 0;
    std::unique_ptr<SoundFontLoader::Result> finishedLoad; // on its way from the loader's thread to the message thread
    juce::String soundFontMessage;
    std::atomic<int> soundFontRevision { 0 };
    SoundFontLoader soundFontLoader;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RetroChipProcessor)
};
} // namespace retrochip
