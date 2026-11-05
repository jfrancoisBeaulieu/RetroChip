// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include "Localisation.h"
#include "PluginProcessor.h"
#include "RetroLookAndFeel.h"
#include "SoundFontLibrary.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace retrochip
{
// What "Open folder" does before showing the folder: makes it, with the parents it needs, if it is not there.
juce::Result createLibraryFolder(const juce::File& root);

// A square button with a drawn icon, a folder or the circling arrow of a refresh. Its tooltip says what it does.
class IconButton final : public juce::Button
{
public:
    enum class Icon
    {
        folder,
        refresh
    };

    explicit IconButton(Icon icon);

    void paintButton(juce::Graphics& g, bool isHighlighted, bool isDown) override;

private:
    Icon icon;
};

// A folder of the user's files beside an import button: a menu of its files, with a heading per first-level folder,
// and buttons that show the folder and read it again. A pick hands the file over, and the menu shows it until the
// editor says the sound in use came from elsewhere. Its parts are the prefix followed by "Library", "OpenFolder"
// and "Refresh".
class LibraryPicker final : public juce::Component
{
public:
    LibraryPicker(const juce::String& idPrefix, Library library, std::function<juce::File()> root,
                  std::function<void(const juce::File&)> onPick);

    // Reads the disk, on the message thread: when the console comes into view, and when the user asks.
    void rescan();
    void openFolder();
    void applyLanguage();

    // Picks the file `delta` places from the one picked last, as the arrow keys do; with none, the first or the last.
    // At either end nothing changes. False when the folder has no file.
    bool step(int delta);
    void clearPick();

    const std::vector<LibraryEntry>& getEntries() const { return entries; }

    void resized() override;

private:
    void pick();
    void showTexts();

    Library library;
    std::function<juce::File()> root;
    std::function<void(const juce::File&)> onPick;
    juce::ComboBox box;
    IconButton openButton { IconButton::Icon::folder };
    IconButton refreshButton { IconButton::Icon::refresh };
    std::vector<LibraryEntry> entries;
    juce::String message;
    juce::String pickedPath; // relative to the folder, so that a scan keeps it
    bool creationFailed = false;
};

// A row of radio buttons bound to a discrete parameter: a choice, or an integer shown as its own numbers.
class SegmentedControl final : public juce::Component
{
public:
    SegmentedControl(juce::RangedAudioParameter& parameter, const juce::String& parameterId, const juce::String& title);

    std::function<void(int)> onChange;

    void resized() override;
    void enablementChanged() override;

private:
    juce::RangedAudioParameter& parameter;
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
    std::unique_ptr<juce::ParameterAttachment> attachment;
};

// The console parameter as the interface shows it: a tab per console, and beside it the console's chips: the NES's
// 2A03 and the VRC6 and N163 of Famicom cartridges, the Genesis's FM and PSG, or the SNES's S-DSP and the SoundFonts.
// A tab goes back to the chip it last had in this editor. Tabs are "tab_0" to "tab_2"; a chip's button is "console_" and its parameter index.
class ConsoleSelector final : public juce::Component
{
public:
    explicit ConsoleSelector(juce::RangedAudioParameter& parameter);

    std::function<void(int)> onChange; // with the parameter's index, after each change

    void applyLanguage();
    void resized() override;

private:
    void select(int console);
    void show(int console);

    std::array<std::unique_ptr<juce::TextButton>, 3> tabs;
    std::array<std::unique_ptr<juce::TextButton>, 7> chips; // by parameter index
    int shownTab = 0;
    std::array<int, 3> lastOfTab { 0, 1, 2 };
    std::unique_ptr<juce::ParameterAttachment> attachment;
};

// A rotary slider with a name above it, bound to a parameter. The title, if given, is the accessible name
// when the label has to be shorter. A compact knob sets its name and value in a smaller type, and its value
// box takes the width of the knob, for the S-DSP's row of twelve.
class Knob final : public juce::Component
{
public:
    Knob(juce::AudioProcessorValueTreeState& apvts, const char* parameterId, const juce::String& name,
         const juce::String& title = {});

    void setCompact(bool shouldBeCompact);
    void refreshText(); // the value again, for a value whose text depends on the language

    void resized() override;
    void enablementChanged() override;

private:
    bool compact = false;
    juce::Label label;
    juce::Slider slider { juce::Slider::RotaryVerticalDrag, juce::Slider::TextBoxBelow };
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;
};

class RetroChipEditor final : public juce::AudioProcessorEditor,
                              private juce::Timer,
                              private juce::ChangeListener,
                              private LanguageListener
{
public:
    explicit RetroChipEditor(RetroChipProcessor& owner);
    ~RetroChipEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // The arrow keys browse the console's sounds once a click has given the window the keyboard: up and down the
    // instruments or timbres (the presets where the chip has none), left and right the files of its folder (the
    // presets where it has no folder). Every other key goes back to the host.
    bool keyPressed(const juce::KeyPress& key) override;
    void mouseDown(const juce::MouseEvent& event) override;

    // What the file chooser does with the file it returns: imports it, and after a successful import
    // selects the user's sample as the instrument, so it can be played at once. A failed import leaves the
    // instrument as it was.
    void importFile(const juce::File& file);

    // The same for the YM2612: a timbre also brings along its algorithm and feedback.
    void importGenesisFile(const juce::File& file);

    // What the chooser hands over. Several files are a kit, and so is one WAV when the kit is the timbre.
    void importGenesisFiles(const juce::Array<juce::File>& files);

    // The same for the NES: an instrument brings its chip into view and turns it on; a WAV goes to the DPCM channel.
    void importNesFile(const juce::File& file);

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void languageChanged() override;
    void applyLanguage(); // every text of the interface, in the current language
    void chooseSample();
    void chooseFiles(const juce::String& title, const juce::String& patterns, int extraFlags, std::function<void(const juce::Array<juce::File>&)> onChosen);
    void updateTimbreControls();
    void updateNesControls();
    int nesChip() const;
    void refreshPresetBox();
    void refreshSampleStatus();
    void updateEnabledControls();
    void showConsole(int consoleIndex);
    void layoutChip2A03();
    void layoutNesKnobs();
    void layoutYm2612();
    void layoutSDsp();
    void layoutSoundFont();
    void layoutPsg();
    void selectTimbre();
    void stepSound(int delta);
    void stepFile(int delta);
    void stepPreset(int delta);
    bool hasTimbre(int index) const;

    // The SoundFont console. The file menu is rebuilt from the disk when the console is shown and when the user
    // asks; the rest follows the processor, which the timer checks for a new revision.
    void rescanLibrary();
    void listLibrary();
    void refreshSoundFontPanel();
    void chooseSoundFontFile();
    void chooseSoundFontInstrument();
    void openLibraryFolder();

    RetroLookAndFeel lookAndFeel;
    RetroChipProcessor& processorRef;
    juce::TooltipWindow tooltipWindow { this }; // without one, no control's tooltip ever appears

    juce::ComboBox presetBox;

    ConsoleSelector consoleControl;

    SegmentedControl channelControl;
    SegmentedControl dutyControl;
    SegmentedControl noiseControl;

    juce::ComboBox timbreBox;
    SegmentedControl algorithmControl;

    SegmentedControl psgChannelControl;
    SegmentedControl psgNoiseTypeControl;
    SegmentedControl psgNoiseRateControl;

    // The NES's cartridge chips. Their envelope is the 2A03's knobs.
    SegmentedControl vrc6ChannelControl;
    SegmentedControl vrc6DutyControl;
    SegmentedControl n163ChannelsControl;
    juce::ComboBox n163WaveBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> n163WaveAttachment;

    Knob attackKnob;
    Knob decayKnob;
    Knob sustainKnob;
    Knob releaseKnob;
    Knob vibratoRateKnob;
    Knob vibratoDepthKnob;
    Knob glideKnob;
    Knob gainKnob;

    Knob feedbackKnob;
    Knob brightnessKnob;
    Knob fmAttackKnob;
    Knob fmDecayKnob;
    Knob fmReleaseKnob;

    Knob psgAttackKnob;
    Knob psgDecayKnob;
    Knob psgSustainKnob;
    Knob psgReleaseKnob;

    juce::ToggleButton velocityToggle { "Velocity" };
    juce::AudioProcessorValueTreeState::ButtonAttachment velocityAttachment;

    // The S-DSP. The instrument menu follows the parameter like the timbre menu. The root note and the loop
    // belong to the user's sample, so they are live only when that is the instrument.
    juce::ComboBox instrumentBox;
    juce::Slider rootNoteSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::AudioProcessorValueTreeState::SliderAttachment rootNoteAttachment;
    juce::ToggleButton loopToggle { "Loop" };
    juce::AudioProcessorValueTreeState::ButtonAttachment loopAttachment;

    // The YM2612's PCM sample has its own. They take the algorithm's place when it is the timbre.
    juce::Slider genRootSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::AudioProcessorValueTreeState::SliderAttachment genRootAttachment;
    juce::ToggleButton genLoopToggle { "Loop" };
    juce::AudioProcessorValueTreeState::ButtonAttachment genLoopAttachment;

    // The 2A03's DPCM channel has these instead of the duty and the noise mode.
    juce::Slider dpcmRootSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::AudioProcessorValueTreeState::SliderAttachment dpcmRootAttachment;
    juce::ToggleButton dpcmLoopToggle { "Loop" };
    juce::AudioProcessorValueTreeState::ButtonAttachment dpcmLoopAttachment;

    // The NES's import, at the top of the envelope panel: an instrument takes the envelope's place.
    juce::ToggleButton nesInstrumentToggle { "Instrument" };
    juce::AudioProcessorValueTreeState::ButtonAttachment nesInstrumentAttachment;

    // The import panel: a button that opens the file chooser, and the processor's status for the sample.
    juce::TextButton importButton;
    juce::Label sampleStatusLabel;
    std::unique_ptr<juce::FileChooser> fileChooser; // alive while its dialog is open
    juce::File lastImportFolder;
    juce::TextButton genImportButton;
    juce::Label genStatusLabel;
    juce::TextButton nesImportButton;
    juce::Label nesStatusLabel;

    // The folders of .tfi timbres and .fti instruments, under the import buttons.
    LibraryPicker genLibrary;
    LibraryPicker nesLibrary;
    bool genLibraryShown = false;
    bool nesLibraryShown = false;
    std::unique_ptr<juce::ParameterAttachment> nesInstrumentWatch; // keeps the envelope's greying in step
    bool nesInstrumentOn = false; // the switch's value, which reaches the watch before the raw value is updated

    Knob snesAttackKnob;
    Knob snesDecayKnob;
    Knob snesSustainLevelKnob;
    Knob snesSustainRateKnob;
    Knob snesReleaseKnob;
    Knob echoMixKnob;
    Knob echoDelayKnob;
    Knob echoFeedbackKnob;

    // The SoundFont console: the file menu (a heading per group of the library), the instruments of the file in
    // use, the two buttons for the folder, and the message. The echo is its own, set apart from the S-DSP's.
    juce::ComboBox soundFontBox;
    juce::ComboBox soundFontInstrumentBox;
    juce::TextButton openFolderButton;
    juce::TextButton refreshButton;
    juce::Label soundFontStatusLabel;
    Knob sfEchoMixKnob;
    Knob sfEchoDelayKnob;
    Knob sfEchoFeedbackKnob;

    juce::MidiKeyboardComponent keyboard;

    // The language, under the keyboard's right end.
    juce::TextButton englishButton { "EN" };
    juce::TextButton frenchButton { "FR" };

    std::unique_ptr<juce::ParameterAttachment> timbreAttachment;
    std::unique_ptr<juce::ParameterAttachment> instrumentAttachment;

    Console shownConsole = Console::Chip2A03;
    int seenProgram = processorRef.getCurrentProgram();
    int channelIndex = 0;
    int instrumentIndex = 0;
    int timbreIndex = 0;
    int psgChannelIndex = 0;
    int psgRateIndex = 0;

    std::vector<LibraryEntry> libraryEntries; // the file menu's items, by id - 1
    juce::String libraryMessage;              // what the scan says about the folder
    juce::String folderMessage;               // what the editor itself has to say, until the processor has news
    std::vector<SoundFontInstrument> listedInstruments;
    int seenSoundFontRevision = -1;
    bool soundFontShown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RetroChipEditor)
};
} // namespace retrochip
