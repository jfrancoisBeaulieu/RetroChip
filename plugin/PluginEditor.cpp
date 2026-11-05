// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "PluginEditor.h"

#include "Parameters.h"
#include "Presets.h"
#include "nes/N163Waves.h"

#include <cmath>
#include <algorithm>

namespace retrochip
{
namespace
{
constexpr int kWidth = 760;
constexpr int kHeight = 520;
constexpr int kMargin = 16;
constexpr int kHeaderHeight = 56;
constexpr int kSelectorHeight = 84;
constexpr int kKeyboardHeight = 110;
constexpr int kGap = 10;
constexpr int kPanelTitleHeight = 22;
constexpr int kRadioGroupBase = 1000;

juce::Rectangle<int> selectorArea()
{
    return { kMargin, kHeaderHeight, kWidth - 2 * kMargin, kSelectorHeight };
}

juce::Rectangle<int> knobArea()
{
    const int top = kHeaderHeight + kSelectorHeight + kGap;
    return { kMargin, top, kWidth - 2 * kMargin, kHeight - top - kKeyboardHeight - kMargin - kGap };
}

// Selector widths are shared with paint(), which draws the panels behind the controls.
juce::Rectangle<int> takeChannelArea(juce::Rectangle<int>& row) { return row.removeFromLeft(320); }
juce::Rectangle<int> takeDutyArea(juce::Rectangle<int>& row) { return row.removeFromLeft(230); }
juce::Rectangle<int> takeVrc6ChannelArea(juce::Rectangle<int>& row) { return row.removeFromLeft(200); }
juce::Rectangle<int> takeN163WaveArea(juce::Rectangle<int>& row) { return row.removeFromLeft(240); }
constexpr int kNesImportStripHeight = 30;
constexpr int kNesLibraryHeight = 26;
juce::Rectangle<int> takePsgChannelArea(juce::Rectangle<int>& row) { return row.removeFromLeft(190); }
juce::Rectangle<int> takePsgNoiseTypeArea(juce::Rectangle<int>& row) { return row.removeFromLeft(240); }

juce::Rectangle<int> takeEnvelopeArea(juce::Rectangle<int>& row) { return row.removeFromLeft(348); }
juce::Rectangle<int> takeMotionArea(juce::Rectangle<int>& row) { return row.removeFromLeft(240); }

// The YM2612 row. Its nine knobs are 68 wide, which leaves the output panel room for the velocity toggle.
constexpr int kFmKnobWidth = 68;
juce::Rectangle<int> takeTimbreArea(juce::Rectangle<int>& row) { return row.removeFromLeft(190); }
juce::Rectangle<int> takeFmImportArea(juce::Rectangle<int>& row) { return row.removeFromLeft(250); }
juce::Rectangle<int> takeFmRootArea(juce::Rectangle<int>& row) { return row.removeFromLeft(140); }
juce::Rectangle<int> takeFmSoundArea(juce::Rectangle<int>& row) { return row.removeFromLeft(2 * kFmKnobWidth + 16); }
juce::Rectangle<int> takeFmEnvelopeArea(juce::Rectangle<int>& row) { return row.removeFromLeft(3 * kFmKnobWidth + 16); }
juce::Rectangle<int> takeFmMotionArea(juce::Rectangle<int>& row) { return row.removeFromLeft(3 * kFmKnobWidth + 16); }

// The S-DSP row. Its 12 knobs are 52 wide in a smaller type; the output panel takes what is left, for the
// gain knob and the velocity toggle. Its panels have less padding than the other consoles'.
constexpr int kSnesKnobWidth = 52;
constexpr int kSnesGainKnobWidth = 60;
constexpr int kSnesPanelPadding = 4;
juce::Rectangle<int> takeSnesInstrumentArea(juce::Rectangle<int>& row) { return row.removeFromLeft(220); }
juce::Rectangle<int> takeSnesImportArea(juce::Rectangle<int>& row) { return row.removeFromLeft(248); }
juce::Rectangle<int> takeSnesRootArea(juce::Rectangle<int>& row) { return row.removeFromLeft(140); }
juce::Rectangle<int> takeSnesEnvelopeArea(juce::Rectangle<int>& row) { return row.removeFromLeft(5 * kSnesKnobWidth + 2 * kSnesPanelPadding); }
juce::Rectangle<int> takeSnesEchoArea(juce::Rectangle<int>& row) { return row.removeFromLeft(3 * kSnesKnobWidth + 2 * kSnesPanelPadding); }
juce::Rectangle<int> takeSnesMotionArea(juce::Rectangle<int>& row) { return row.removeFromLeft(3 * kSnesKnobWidth + 2 * kSnesPanelPadding); }

// The SoundFont row. The folder panel stacks its two buttons; the message takes the rest of the row. Of the knobs,
// the echo's three are in the S-DSP's compact style, a little wider; the output panel is as wide as its gain knob.
constexpr int kSfKnobWidth = 72;
constexpr int kSfButtonHeight = 24;
juce::Rectangle<int> takeSfFileArea(juce::Rectangle<int>& row) { return row.removeFromLeft(196); }
juce::Rectangle<int> takeSfInstrumentArea(juce::Rectangle<int>& row) { return row.removeFromLeft(176); }
juce::Rectangle<int> takeSfFolderArea(juce::Rectangle<int>& row) { return row.removeFromLeft(140); }
juce::Rectangle<int> takeSfEchoArea(juce::Rectangle<int>& row) { return row.removeFromLeft(3 * kSfKnobWidth + 16); }
juce::Rectangle<int> takeSfOutputArea(juce::Rectangle<int>& row) { return row.removeFromLeft(140); }

// The import panels' two rows: the import button with its status, and the folder under it.
constexpr int kImportRowHeight = 24;

// The language switch, at the right end of the legal notice's line.
constexpr int kLanguageButtonWidth = 26;
juce::Rectangle<int> languageArea() { return { kWidth - kMargin - 2 * kLanguageButtonWidth, kHeight - kMargin + 1, 2 * kLanguageButtonWidth, kMargin - 2 }; }

// A choice's menu, in the language of the parameter's texts. The parameter is the editor's to keep in step with.
void fillChoices(juce::ComboBox& box, juce::RangedAudioParameter& parameter, int selectedIndex)
{
    box.clear(juce::dontSendNotification);
    for (int i = 0; i < parameter.getNumSteps(); ++i)
        box.addItem(parameter.getText(parameter.convertTo0to1(static_cast<float>(i)), 1024), i + 1);
    box.setSelectedItemIndex(selectedIndex, juce::dontSendNotification);
}

// Keeps the other parameters of a timbre change in step with it, as one gesture each.
void setParameterValue(juce::RangedAudioParameter& parameter, float value)
{
    parameter.beginChangeGesture();
    parameter.setValueNotifyingHost(parameter.convertTo0to1(value));
    parameter.endChangeGesture();
}

void drawPanel(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    g.setColour(palette::panel);
    g.fillRect(area);
    g.setColour(palette::panelEdge);
    g.drawRect(area, 1);
    g.setColour(palette::dimText);
    g.setFont(RetroLookAndFeel::retroFont(12.0f, true));
    g.drawText(title, area.withHeight(kPanelTitleHeight).reduced(8, 0), juce::Justification::centredLeft);
}

juce::Rectangle<int> panelContent(juce::Rectangle<int> area, int padding = 8)
{
    return area.withTrimmedTop(kPanelTitleHeight).reduced(padding, 4);
}
} // namespace

juce::Result createLibraryFolder(const juce::File& root)
{
    if (root == juce::File())
        return juce::Result::fail("No folder");
    if (root.isDirectory())
        return juce::Result::ok();
    return root.createDirectory();
}

IconButton::IconButton(Icon shown)
    : juce::Button({}), icon(shown)
{
}

void IconButton::paintButton(juce::Graphics& g, bool isHighlighted, bool isDown)
{
    getLookAndFeel().drawButtonBackground(g, *this, findColour(juce::TextButton::buttonColourId), isHighlighted, isDown);

    // Drawn in a 14-point square in the middle, in the text's colour.
    const auto box = getLocalBounds().toFloat().withSizeKeepingCentre(14.0f, 14.0f);
    g.setColour(isEnabled() ? palette::text : palette::dimText);
    const juce::PathStrokeType stroke(1.6f, juce::PathStrokeType::mitered, juce::PathStrokeType::square);

    if (icon == Icon::folder)
    {
        juce::Path folder;
        folder.startNewSubPath(box.getX(), box.getY() + 2.5f);
        folder.lineTo(box.getX() + 5.0f, box.getY() + 2.5f);
        folder.lineTo(box.getX() + 6.5f, box.getY() + 4.5f);
        folder.lineTo(box.getRight(), box.getY() + 4.5f);
        folder.lineTo(box.getRight(), box.getBottom() - 1.5f);
        folder.lineTo(box.getX(), box.getBottom() - 1.5f);
        folder.closeSubPath();
        g.strokePath(folder, stroke);
        return;
    }

    // Three quarters of a circle, clockwise, and an arrowhead where it ends. Angles start at noon.
    const auto centre = box.getCentre();
    constexpr float radius = 5.5f;
    constexpr float start = 0.9f;
    constexpr float end = juce::MathConstants<float>::twoPi - 0.35f;
    juce::Path arc;
    arc.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, start, end, true);
    g.strokePath(arc, juce::PathStrokeType(1.6f));

    const juce::Point<float> tip { centre.x + radius * std::sin(end), centre.y - radius * std::cos(end) };
    const juce::Point<float> along { std::cos(end), std::sin(end) };  // the way the arc goes
    const juce::Point<float> outward { std::sin(end), -std::cos(end) };
    juce::Path head;
    head.addTriangle(tip + along * 3.5f, tip + outward * 3.0f, tip - outward * 3.0f);
    g.fillPath(head);
}

LibraryPicker::LibraryPicker(const juce::String& idPrefix, Library kind, std::function<juce::File()> folder,
                             std::function<void(const juce::File&)> picked)
    : library(kind), root(std::move(folder)), onPick(std::move(picked))
{
    setComponentID(idPrefix + "LibraryPanel");
    const juce::String format = library == Library::tfi ? "TFI" : "FTI";
    setTitle(format + " folder");
    setFocusContainerType(FocusContainerType::focusContainer);

    box.setComponentID(idPrefix + "Library");
    box.setTitle(format + " files of the folder");
    box.onChange = [this] { pick(); };
    addAndMakeVisible(box);

    openButton.setComponentID(idPrefix + "OpenFolder");
    openButton.setTitle("Open the " + format + " folder");
    openButton.onClick = [this] { openFolder(); };
    addAndMakeVisible(openButton);

    refreshButton.setComponentID(idPrefix + "Refresh");
    refreshButton.setTitle("Refresh the " + format + " list");
    refreshButton.onClick = [this] { rescan(); };
    addAndMakeVisible(refreshButton);

    showTexts();
}

void LibraryPicker::rescan()
{
    creationFailed = false;
    entries = scanLibrary(root(), message, library);

    box.clear(juce::dontSendNotification);
    juce::String group;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (i == 0 || entries[i].group != group)
        {
            group = entries[i].group;
            box.addSectionHeading(group);
        }
        box.addItem(entries[i].name, static_cast<int>(i) + 1);
    }

    int picked = 0;
    for (size_t i = 0; i < entries.size() && picked == 0; ++i)
        if (entries[i].relativePath == pickedPath)
            picked = static_cast<int>(i) + 1;
    if (picked == 0)
        pickedPath.clear();
    box.setSelectedId(picked, juce::dontSendNotification);
    showTexts();
}

// The folder is made if it is not there, shown, and read again. If it cannot be made, the menu says so.
void LibraryPicker::openFolder()
{
    const auto folder = root();
    const auto made = createLibraryFolder(folder);
    if (made.wasOk())
        folder.revealToUser();
    rescan();
    creationFailed = made.failed();
    showTexts();
}

void LibraryPicker::applyLanguage()
{
    rescan(); // the group of loose files and the messages have the language's words
}

// The file may have gone since the scan; then the import says it cannot be read.
void LibraryPicker::pick()
{
    const int index = box.getSelectedId() - 1;
    if (index < 0 || index >= static_cast<int>(entries.size()))
        return;
    pickedPath = entries[static_cast<size_t>(index)].relativePath;
    if (onPick != nullptr)
        if (const auto file = resolveInLibrary(root(), pickedPath); file != juce::File())
            onPick(file);
}

bool LibraryPicker::step(int delta)
{
    if (entries.empty())
        return false;
    const int last = static_cast<int>(entries.size()) - 1;
    const int current = box.getSelectedId() - 1;
    const int next = current < 0 ? (delta > 0 ? 0 : last) : juce::jlimit(0, last, current + delta);
    if (next != current)
        box.setSelectedId(next + 1, juce::sendNotificationSync);
    return true;
}

void LibraryPicker::clearPick()
{
    pickedPath.clear();
    box.setSelectedId(0, juce::dontSendNotification);
}

void LibraryPicker::showTexts()
{
    const auto folder = root();
    box.setTextWhenNothingSelected(creationFailed ? tr("Folder cannot be created") : entries.empty() ? tr("Empty folder") : tr("From the folder"));
    box.setTextWhenNoChoicesAvailable(message);
    box.setTooltip(creationFailed ? tr("Folder cannot be created: ") + folder.getFileName()
                   : message.isNotEmpty() ? message
                                          : tr("Folder: ") + folder.getFullPathName());
    openButton.setTooltip(library == Library::tfi ? tr("Opens the folder of .tfi timbres, and creates it if it does not exist")
                                                  : tr("Opens the folder of .fti instruments, and creates it if it does not exist"));
    refreshButton.setTooltip(tr("Reads the folder again to find the files added"));
}

void LibraryPicker::resized()
{
    auto area = getLocalBounds();
    const int side = area.getHeight();
    refreshButton.setBounds(area.removeFromRight(side));
    area.removeFromRight(4);
    openButton.setBounds(area.removeFromRight(side));
    area.removeFromRight(4);
    box.setBounds(area);
}

SegmentedControl::SegmentedControl(juce::RangedAudioParameter& param, const juce::String& parameterId, const juce::String& title)
    : parameter(param)
{
    static int nextRadioGroup = kRadioGroupBase;
    const int radioGroup = nextRadioGroup++;

    setComponentID(parameterId);
    setTitle(title);
    setFocusContainerType(FocusContainerType::focusContainer);

    // Button i stands for the i-th step of the parameter, whatever its range: a choice, or 1 to 8.
    const int steps = juce::jmax(2, parameter.getNumSteps());
    const auto valueOf = [this, steps](int i) { return parameter.convertFrom0to1(static_cast<float>(i) / static_cast<float>(steps - 1)); };

    for (int i = 0; i < steps; ++i)
    {
        const auto text = parameter.getText(static_cast<float>(i) / static_cast<float>(steps - 1), 1024);
        auto button = std::make_unique<juce::TextButton>(text);
        button->setComponentID(parameterId + "_" + juce::String(i));
        button->setTitle(title + ": " + text);
        button->setClickingTogglesState(true);
        button->setRadioGroupId(radioGroup);
        button->onClick = [this, i, valueOf, raw = button.get()]
        {
            if (raw->getToggleState())
                attachment->setValueAsCompleteGesture(valueOf(i));
        };
        addAndMakeVisible(*button);
        buttons.push_back(std::move(button));
    }

    attachment = std::make_unique<juce::ParameterAttachment>(
        parameter,
        [this, steps](float value)
        {
            const int index = juce::jlimit(0, steps - 1,
                                           juce::roundToInt(parameter.convertTo0to1(value) * static_cast<float>(steps - 1)));
            buttons[static_cast<size_t>(index)]->setToggleState(true, juce::dontSendNotification);
            if (onChange != nullptr)
                onChange(index);
        },
        nullptr);
    attachment->sendInitialUpdate();
}

void SegmentedControl::resized()
{
    auto area = getLocalBounds();
    const int width = area.getWidth() / static_cast<int>(buttons.size());
    for (auto& button : buttons)
        button->setBounds(area.removeFromLeft(width));
}

namespace
{
constexpr int kConsoleTabsWidth = 210;
constexpr int kConsoleChipsGap = 8;

// The console parameter's indexes under each tab, in the order their buttons go: the NES (2A03, VRC6, N163), the
// Genesis (FM, PSG) and the SNES (S-DSP, SoundFont).
const std::vector<int>& consolesOfTab(int tab)
{
    static const std::vector<int> tabs[] = { { 0, 5, 6 }, { 1, 4 }, { 2, 3 } };
    return tabs[juce::jlimit(0, 2, tab)];
}

struct SelectorEntry
{
    const char* text;
    const char* tooltip; // a key of tr()
};

constexpr SelectorEntry kTabEntries[] = { { "NES", "The NES's 2A03 chip, and the VRC6 and Namco 163 chips of Famicom cartridges" },
                                          { "Genesis", "Two chips: YM2612 (FM) and SN76489 (PSG)" },
                                          { "SNES", "The SNES's S-DSP chip, or SoundFont files" } };

// By the console parameter's index.
constexpr SelectorEntry kChipEntries[] = { { "2A03", "The NES's 2A03 chip: pulse, triangle, noise and DPCM" },
                                           { "FM", "The Genesis's YM2612 chip: FM synthesis, TFI timbres, PCM" },
                                           { "S-DSP", "The SNES's S-DSP chip" },
                                           { "SoundFont", "Player for .sf2 files" },
                                           { "PSG", "The Genesis's SN76489 chip: square waves and noise" },
                                           { "VRC6", "Konami's VRC6 chip, in Famicom cartridges: two pulses and a sawtooth" },
                                           { "N163", "The Namco 163 chip, in Famicom cartridges: drawn waveforms" } };

int tabOf(int console)
{
    for (int tab = 0; tab < 3; ++tab)
        for (const int c : consolesOfTab(tab))
            if (c == console)
                return tab;
    return 0;
}
} // namespace

ConsoleSelector::ConsoleSelector(juce::RangedAudioParameter& parameter)
{
    setComponentID(ids::console);
    setTitle("Console");
    setFocusContainerType(FocusContainerType::focusContainer);

    for (size_t i = 0; i < tabs.size(); ++i)
    {
        auto& tab = tabs[i];
        tab = std::make_unique<juce::TextButton>(kTabEntries[i].text);
        tab->setComponentID("tab_" + juce::String(static_cast<int>(i)));
        tab->setTitle(juce::String("Console: ") + kTabEntries[i].text);
        tab->onClick = [this, i] { select(lastOfTab[i]); };
        addAndMakeVisible(*tab);
    }

    for (int i = 0; i < static_cast<int>(chips.size()); ++i)
    {
        auto& chip = chips[static_cast<size_t>(i)];
        chip = std::make_unique<juce::TextButton>(kChipEntries[i].text);
        chip->setComponentID("console_" + juce::String(i));
        chip->setTitle(juce::String("Chip: ") + kChipEntries[i].text);
        chip->onClick = [this, i] { select(i); };
        addChildComponent(*chip);
    }
    applyLanguage();

    attachment = std::make_unique<juce::ParameterAttachment>(parameter, [this](float value) { show(juce::roundToInt(value)); }, nullptr);
    attachment->sendInitialUpdate();
}

void ConsoleSelector::applyLanguage()
{
    for (size_t i = 0; i < tabs.size(); ++i)
        tabs[i]->setTooltip(tr(kTabEntries[i].tooltip));
    for (size_t i = 0; i < chips.size(); ++i)
        chips[i]->setTooltip(tr(kChipEntries[i].tooltip));
}

void ConsoleSelector::select(int console)
{
    attachment->setValueAsCompleteGesture(static_cast<float>(console));
}

void ConsoleSelector::show(int console)
{
    console = juce::jlimit(0, static_cast<int>(chips.size()) - 1, console);
    shownTab = tabOf(console);
    lastOfTab[static_cast<size_t>(shownTab)] = console;

    for (size_t i = 0; i < tabs.size(); ++i)
        tabs[i]->setToggleState(static_cast<int>(i) == shownTab, juce::dontSendNotification);
    for (int i = 0; i < static_cast<int>(chips.size()); ++i)
    {
        auto& chip = *chips[static_cast<size_t>(i)];
        chip.setVisible(tabOf(i) == shownTab);
        chip.setToggleState(i == console, juce::dontSendNotification);
    }
    resized();

    if (onChange != nullptr)
        onChange(console);
}

void ConsoleSelector::resized()
{
    auto area = getLocalBounds();
    auto tabArea = area.removeFromLeft(kConsoleTabsWidth);
    const int tabWidth = tabArea.getWidth() / static_cast<int>(tabs.size());
    for (auto& tab : tabs)
        tab->setBounds(tabArea.removeFromLeft(tabWidth));

    // The tab's chips share what is left, in their order.
    area.removeFromLeft(kConsoleChipsGap);
    const auto& shown = consolesOfTab(shownTab);
    const int chipWidth = area.getWidth() / static_cast<int>(shown.size());
    for (size_t i = 0; i < shown.size(); ++i)
    {
        auto bounds = area.removeFromLeft(chipWidth);
        if (i + 1 == shown.size())
            bounds = bounds.withRight(getWidth());
        chips[static_cast<size_t>(shown[i])]->setBounds(bounds);
    }
}

void SegmentedControl::enablementChanged()
{
    setAlpha(isEnabled() ? 1.0f : 0.35f);
}

Knob::Knob(juce::AudioProcessorValueTreeState& apvts, const char* parameterId, const juce::String& name, const juce::String& title)
    : attachment(apvts, parameterId, slider)
{
    setComponentID(parameterId);

    label.setText(name, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(label);

    slider.setTitle(title.isEmpty() ? name : title);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 68, 20);
    addAndMakeVisible(slider);
}

void Knob::setCompact(bool shouldBeCompact)
{
    if (compact == shouldBeCompact)
        return;

    compact = shouldBeCompact;
    label.getProperties().set("compact", compact);
    slider.getProperties().set("compact", compact);
    resized();
    repaint();
}

void Knob::refreshText()
{
    slider.updateText();
}

void Knob::resized()
{
    constexpr int labelHeight = 20;
    constexpr int knobHeight = 72;
    constexpr int textBoxHeight = 22;

    // The value box is as wide as the knob when that is narrower than the usual 68.
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, compact ? getWidth() : 68, 20);

    auto area = getLocalBounds().withSizeKeepingCentre(getWidth(), labelHeight + knobHeight + textBoxHeight);
    label.setBounds(area.removeFromTop(labelHeight));
    slider.setBounds(area);
}

void Knob::enablementChanged()
{
    setAlpha(isEnabled() ? 1.0f : 0.35f);
}

RetroChipEditor::RetroChipEditor(RetroChipProcessor& owner)
    : AudioProcessorEditor(owner),
      processorRef(owner),
      consoleControl(*owner.apvts.getParameter(ids::console)),
      channelControl(*owner.apvts.getParameter(ids::channel), ids::channel, "Channel"),
      dutyControl(*owner.apvts.getParameter(ids::duty), ids::duty, "Duty"),
      noiseControl(*owner.apvts.getParameter(ids::noiseMode), ids::noiseMode, "Noise mode"),
      algorithmControl(*owner.apvts.getParameter(ids::genAlgorithm), ids::genAlgorithm, "Algorithm"),
      psgChannelControl(*owner.apvts.getParameter(ids::psgChannel), ids::psgChannel, "PSG channel"),
      psgNoiseTypeControl(*owner.apvts.getParameter(ids::psgNoiseType), ids::psgNoiseType, "PSG noise type"),
      psgNoiseRateControl(*owner.apvts.getParameter(ids::psgNoiseRate), ids::psgNoiseRate, "PSG noise rate"),
      vrc6ChannelControl(*owner.apvts.getParameter(ids::vrc6Channel), ids::vrc6Channel, "VRC6 channel"),
      vrc6DutyControl(*owner.apvts.getParameter(ids::vrc6Duty), ids::vrc6Duty, "VRC6 duty"),
      n163ChannelsControl(*owner.apvts.getParameter(ids::n163Channels), ids::n163Channels, "N163 channels"),
      attackKnob(owner.apvts, ids::attack, "Attack"),
      decayKnob(owner.apvts, ids::decay, "Decay"),
      sustainKnob(owner.apvts, ids::sustain, "Sustain"),
      releaseKnob(owner.apvts, ids::release, "Release"),
      vibratoRateKnob(owner.apvts, ids::vibratoRate, "Rate", "Vibrato Rate"),
      vibratoDepthKnob(owner.apvts, ids::vibratoDepth, "Depth", "Vibrato Depth"),
      glideKnob(owner.apvts, ids::glide, "Glide"),
      gainKnob(owner.apvts, ids::gain, "Gain"),
      feedbackKnob(owner.apvts, ids::genFeedback, "Feedback"),
      brightnessKnob(owner.apvts, ids::genBrightness, "Bright", "Brightness"),
      fmAttackKnob(owner.apvts, ids::genAttack, "Attack"),
      fmDecayKnob(owner.apvts, ids::genDecay, "Decay"),
      fmReleaseKnob(owner.apvts, ids::genRelease, "Release"),
      psgAttackKnob(owner.apvts, ids::psgAttack, "Attack"),
      psgDecayKnob(owner.apvts, ids::psgDecay, "Decay"),
      psgSustainKnob(owner.apvts, ids::psgSustain, "Sustain"),
      psgReleaseKnob(owner.apvts, ids::psgRelease, "Release"),
      velocityAttachment(owner.apvts, ids::velocity, velocityToggle),
      rootNoteAttachment(owner.apvts, ids::snesRootNote, rootNoteSlider),
      loopAttachment(owner.apvts, ids::snesLoop, loopToggle),
      genRootAttachment(owner.apvts, ids::genRootNote, genRootSlider),
      genLoopAttachment(owner.apvts, ids::genLoop, genLoopToggle),
      dpcmRootAttachment(owner.apvts, ids::dpcmRootNote, dpcmRootSlider),
      dpcmLoopAttachment(owner.apvts, ids::dpcmLoop, dpcmLoopToggle),
      nesInstrumentAttachment(owner.apvts, ids::nesUseInstrument, nesInstrumentToggle),
      genLibrary("gen", Library::tfi, [&owner] { return owner.getLibraryRoot(Library::tfi); },
                 [this](const juce::File& file) { importGenesisFile(file); }),
      nesLibrary("nes", Library::fti, [&owner] { return owner.getLibraryRoot(Library::fti); },
                 [this](const juce::File& file) { importNesFile(file); }),
      snesAttackKnob(owner.apvts, ids::snesAttack, "Attack"),
      snesDecayKnob(owner.apvts, ids::snesDecay, "Decay"),
      snesSustainLevelKnob(owner.apvts, ids::snesSustainLevel, "S Level", "Sustain Level"),
      snesSustainRateKnob(owner.apvts, ids::snesSustainRate, "S Rate", "Sustain Rate"),
      snesReleaseKnob(owner.apvts, ids::snesRelease, "Release"),
      echoMixKnob(owner.apvts, ids::snesEchoMix, "Mix", "Echo Mix"),
      echoDelayKnob(owner.apvts, ids::snesEchoDelay, "Delay", "Echo Delay"),
      echoFeedbackKnob(owner.apvts, ids::snesEchoFeedback, "Fdbk", "Echo Feedback"),
      sfEchoMixKnob(owner.apvts, ids::sfEchoMix, "Mix", "Echo Mix"),
      sfEchoDelayKnob(owner.apvts, ids::sfEchoDelay, "Delay", "Echo Delay"),
      sfEchoFeedbackKnob(owner.apvts, ids::sfEchoFeedback, "Fdbk", "Echo Feedback"),
      keyboard(owner.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    setLookAndFeel(&lookAndFeel);

    presetBox.setComponentID("preset");
    presetBox.setTitle("Preset");
    // The menu is grouped by console, under the selector's names: a heading each time the console changes.
    const auto& consoleNames = dynamic_cast<juce::AudioParameterChoice&>(*owner.apvts.getParameter(ids::console)).choices;
    int menuConsole = -1;
    for (int i = 0; i < owner.getNumPrograms(); ++i)
    {
        const int console = kPresets[static_cast<size_t>(i)].console;
        if (console != menuConsole)
        {
            presetBox.addSectionHeading(consoleNames[console]);
            menuConsole = console;
        }
        presetBox.addItem(owner.getProgramName(i), i + 1);
    }
    presetBox.onChange = [this]
    {
        const int index = presetBox.getSelectedItemIndex();
        if (index >= 0)
            processorRef.setCurrentProgram(index);
        refreshPresetBox(); // a console change on the way has cleared it
    };
    addAndMakeVisible(presetBox);

    addAndMakeVisible(consoleControl);
    addAndMakeVisible(channelControl);
    addAndMakeVisible(dutyControl);
    addAndMakeVisible(noiseControl);
    addAndMakeVisible(algorithmControl);
    addAndMakeVisible(psgChannelControl);
    addAndMakeVisible(psgNoiseTypeControl);
    addAndMakeVisible(psgNoiseRateControl);
    addAndMakeVisible(vrc6ChannelControl);
    addAndMakeVisible(vrc6DutyControl);
    addAndMakeVisible(n163ChannelsControl);

    n163WaveBox.setComponentID(ids::n163Wave);
    n163WaveBox.setTitle("N163 wave");
    for (int i = 0; i < kN163WaveCount; ++i)
        n163WaveBox.addItem(kN163WaveNames[i], i + 1);
    n163WaveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(owner.apvts, ids::n163Wave, n163WaveBox);
    addAndMakeVisible(n163WaveBox);

    dpcmRootSlider.setComponentID(ids::dpcmRootNote);
    dpcmRootSlider.setTitle("DPCM root note");
    dpcmRootSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 52, 28);
    addAndMakeVisible(dpcmRootSlider);
    dpcmLoopToggle.setComponentID(ids::dpcmLoop);
    dpcmLoopToggle.setTitle("DPCM loop");
    addAndMakeVisible(dpcmLoopToggle);

    nesInstrumentToggle.setComponentID(ids::nesUseInstrument);
    nesInstrumentToggle.setTitle("Use the FTI instrument");
    addAndMakeVisible(nesInstrumentToggle);
    nesInstrumentWatch = std::make_unique<juce::ParameterAttachment>(
        *owner.apvts.getParameter(ids::nesUseInstrument),
        [this](float value)
        {
            nesInstrumentOn = value >= 0.5f;
            updateEnabledControls();
            refreshSampleStatus();
        },
        nullptr);
    nesInstrumentWatch->sendInitialUpdate();

    nesImportButton.setComponentID("nesImport");
    nesImportButton.setTitle("Import an FTI or WAV file");
    nesImportButton.onClick = [this]
    {
        chooseFiles(tr("Import an .fti instrument or a WAV"), "*.fti;*.wav;*.wave", 0,
                    [this](const juce::Array<juce::File>& files)
                    {
                        nesLibrary.clearPick();
                        importNesFile(files.getFirst());
                    });
    };
    addAndMakeVisible(nesImportButton);
    nesStatusLabel.setComponentID("nesStatus");
    nesStatusLabel.setTitle("NES import status");
    nesStatusLabel.setJustificationType(juce::Justification::centredLeft);
    nesStatusLabel.getProperties().set("compact", true);
    addAndMakeVisible(nesStatusLabel);
    addAndMakeVisible(nesLibrary);

    timbreBox.setComponentID(ids::genPatch);
    timbreBox.setTitle("Timbre");
    fillChoices(timbreBox, *owner.apvts.getParameter(ids::genPatch), 0);
    addAndMakeVisible(timbreBox);

    genRootSlider.setComponentID(ids::genRootNote);
    genRootSlider.setTitle("PCM root note");
    genRootSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 52, 28);
    addAndMakeVisible(genRootSlider);

    genLoopToggle.setComponentID(ids::genLoop);
    genLoopToggle.setTitle("PCM loop");
    addAndMakeVisible(genLoopToggle);

    genImportButton.setComponentID("genImport");
    genImportButton.setTitle("Import a TFI or WAV file");
    genImportButton.onClick = [this]
    {
        chooseFiles(tr("Import a TFI, a WAV or several WAVs"), "*.tfi;*.wav;*.wave",
                    juce::FileBrowserComponent::canSelectMultipleItems,
                    [this](const juce::Array<juce::File>& files)
                    {
                        genLibrary.clearPick();
                        importGenesisFiles(files);
                    });
    };
    addAndMakeVisible(genImportButton);

    genStatusLabel.setComponentID("genStatus");
    genStatusLabel.setTitle("Import status");
    genStatusLabel.setJustificationType(juce::Justification::centredLeft);
    genStatusLabel.getProperties().set("compact", true);
    addAndMakeVisible(genStatusLabel);
    addAndMakeVisible(genLibrary);

    for (auto* knob : { &attackKnob, &decayKnob, &sustainKnob, &releaseKnob, &vibratoRateKnob, &vibratoDepthKnob, &glideKnob, &gainKnob,
                        &feedbackKnob, &brightnessKnob, &fmAttackKnob, &fmDecayKnob, &fmReleaseKnob,
                        &psgAttackKnob, &psgDecayKnob, &psgSustainKnob, &psgReleaseKnob,
                        &snesAttackKnob, &snesDecayKnob, &snesSustainLevelKnob, &snesSustainRateKnob, &snesReleaseKnob,
                        &echoMixKnob, &echoDelayKnob, &echoFeedbackKnob, &sfEchoMixKnob, &sfEchoDelayKnob, &sfEchoFeedbackKnob })
        addAndMakeVisible(*knob);

    instrumentBox.setComponentID(ids::snesInstrument);
    instrumentBox.setTitle("Instrument");
    fillChoices(instrumentBox, *owner.apvts.getParameter(ids::snesInstrument), 0);
    addAndMakeVisible(instrumentBox);

    rootNoteSlider.setComponentID(ids::snesRootNote);
    rootNoteSlider.setTitle("Root note");
    rootNoteSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 52, 28);
    addAndMakeVisible(rootNoteSlider);

    loopToggle.setComponentID(ids::snesLoop);
    loopToggle.setTitle("Loop");
    addAndMakeVisible(loopToggle);

    importButton.setComponentID("snesImport");
    importButton.setTitle("Import WAV file");
    importButton.onClick = [this] { chooseSample(); };
    addAndMakeVisible(importButton);

    sampleStatusLabel.setComponentID("snesSampleStatus");
    sampleStatusLabel.setTitle("Sample status");
    sampleStatusLabel.setJustificationType(juce::Justification::centredLeft);
    sampleStatusLabel.getProperties().set("compact", true); // the smaller type, for a message of two lines
    addAndMakeVisible(sampleStatusLabel);
    refreshSampleStatus();
    owner.sampleChanges.addChangeListener(this);

    // The SoundFont console. The files are in a menu with a heading per folder; the folder and the message are
    // beside it. The message is a label, so that a tooltip can carry what the panel has no room for.
    soundFontBox.setComponentID("sfFile");
    soundFontBox.setTitle("SoundFont file");
    soundFontBox.onChange = [this] { chooseSoundFontFile(); };
    addAndMakeVisible(soundFontBox);

    soundFontInstrumentBox.setComponentID("sfInstrument");
    soundFontInstrumentBox.setTitle("SoundFont instrument");
    soundFontInstrumentBox.onChange = [this] { chooseSoundFontInstrument(); };
    addAndMakeVisible(soundFontInstrumentBox);

    openFolderButton.setComponentID("sfOpenFolder");
    openFolderButton.setTitle("Open the SoundFont folder");
    openFolderButton.onClick = [this] { openLibraryFolder(); };
    addAndMakeVisible(openFolderButton);

    refreshButton.setComponentID("sfRefresh");
    refreshButton.setTitle("Refresh the SoundFont list");
    refreshButton.onClick = [this] { rescanLibrary(); };
    addAndMakeVisible(refreshButton);

    soundFontStatusLabel.setComponentID("sfStatus");
    soundFontStatusLabel.setTitle("SoundFont status");
    soundFontStatusLabel.setJustificationType(juce::Justification::centredLeft);
    soundFontStatusLabel.getProperties().set("compact", true);
    addAndMakeVisible(soundFontStatusLabel);

    velocityToggle.setComponentID(ids::velocity);
    velocityToggle.setTitle("Velocity");
    addAndMakeVisible(velocityToggle);

    keyboard.setTitle("Keyboard");
    keyboard.setAvailableRange(36, 96);
    keyboard.setLowestVisibleKey(36);
    keyboard.setScrollButtonsVisible(false);
    keyboard.setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, palette::accent);
    keyboard.setColour(juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, palette::accent.withAlpha(0.3f));
    addAndMakeVisible(keyboard);

    // The language is the user's for every instance, so a click here changes every open editor.
    constexpr int kLanguageRadioGroup = 900;
    const auto setUpLanguageButton = [this](juce::TextButton& button, const char* id, const char* title, Language language)
    {
        button.setComponentID(id);
        button.setTitle(juce::String::fromUTF8(title));
        button.setTooltip(juce::String::fromUTF8(title));
        button.setRadioGroupId(kLanguageRadioGroup);
        button.setClickingTogglesState(true);
        button.getProperties().set("compact", true);
        button.onClick = [language]
        {
            if (currentLanguage() != language)
                setLanguage(language);
        };
        addAndMakeVisible(button);
    };
    setUpLanguageButton(englishButton, "language_en", "English", Language::english);
    setUpLanguageButton(frenchButton, "language_fr", "Français", Language::french);
    addLanguageListener(this);

    // The timbre menu follows the parameter, which the host can automate. Choosing a timbre here also
    // brings along its algorithm and feedback; automating the timbre alone leaves them where they are.
    timbreAttachment = std::make_unique<juce::ParameterAttachment>(
        *owner.apvts.getParameter(ids::genPatch),
        [this](float value)
        {
            timbreIndex = juce::roundToInt(value);
            timbreBox.setSelectedItemIndex(timbreIndex, juce::dontSendNotification);
            if (timbreIndex != GenesisEngine::kUserPatch)
                genLibrary.clearPick(); // what the folder's menu shows is no longer the timbre
            updateTimbreControls();
            updateEnabledControls();
            refreshSampleStatus(); // the invitation to import depends on the timbre
            repaint();
        },
        nullptr);
    timbreAttachment->sendInitialUpdate();
    timbreBox.onChange = [this] { selectTimbre(); };

    // The same for the instrument. It also decides whether the root note and the loop are live.
    instrumentAttachment = std::make_unique<juce::ParameterAttachment>(
        *owner.apvts.getParameter(ids::snesInstrument),
        [this](float value)
        {
            instrumentIndex = juce::roundToInt(value);
            instrumentBox.setSelectedItemIndex(instrumentIndex, juce::dontSendNotification);
            updateEnabledControls();
            refreshSampleStatus(); // the invitation to import depends on the instrument
        },
        nullptr);
    instrumentAttachment->sendInitialUpdate();
    instrumentBox.onChange = [this]
    {
        const int index = instrumentBox.getSelectedItemIndex();
        if (index >= 0)
            instrumentAttachment->setValueAsCompleteGesture(static_cast<float>(index));
    };

    channelIndex = static_cast<int>(owner.apvts.getRawParameterValue(ids::channel)->load());

    channelControl.onChange = [this](int index)
    {
        channelIndex = index;
        updateNesControls();
        updateEnabledControls();
        refreshSampleStatus();
    };

    psgChannelControl.onChange = [this](int index)
    {
        psgChannelIndex = index;
        updateEnabledControls();
    };
    psgChannelIndex = juce::roundToInt(owner.apvts.getRawParameterValue(ids::psgChannel)->load());
    psgNoiseRateControl.onChange = [this](int index)
    {
        psgRateIndex = index;
        updateEnabledControls();
    };
    psgRateIndex = juce::roundToInt(owner.apvts.getRawParameterValue(ids::psgNoiseRate)->load());

    // The window takes the keyboard for the arrows when it is clicked. A click on a button or a menu leaves the
    // keyboard with the window, so that Return does not click it again; the on-screen keyboard never takes it, so the
    // letters stay the host's. Tab still reaches every control.
    setWantsKeyboardFocus(true);
    keyboard.setWantsKeyboardFocus(false);
    std::function<void(juce::Component&)> keepFocusOnClick = [&keepFocusOnClick](juce::Component& component)
    {
        for (auto* child : component.getChildren())
        {
            if (dynamic_cast<juce::Button*>(child) != nullptr || dynamic_cast<juce::ComboBox*>(child) != nullptr)
                child->setMouseClickGrabsKeyboardFocus(false);
            keepFocusOnClick(*child);
        }
    };
    keepFocusOnClick(*this);
    addMouseListener(this, true);

    applyLanguage();
    consoleControl.onChange = [this](int index) { showConsole(index); };
    showConsole(static_cast<int>(owner.getActiveConsole()));

    setSize(kWidth, kHeight);
    startTimerHz(10);
}

RetroChipEditor::~RetroChipEditor()
{
    removeLanguageListener(this);
    processorRef.sampleChanges.removeChangeListener(this);
    fileChooser.reset(); // closes the dialog if it is still open
    setLookAndFeel(nullptr);
}

void RetroChipEditor::changeListenerCallback(juce::ChangeBroadcaster*)
{
    refreshSampleStatus();
    updateEnabledControls(); // an import may give the chip an instrument to switch on
}

bool RetroChipEditor::keyPressed(const juce::KeyPress& key)
{
    if (key.getModifiers().isAnyModifierKeyDown())
        return false; // the host's shortcuts
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        stepSound(code == juce::KeyPress::downKey ? 1 : -1);
        return true;
    }
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        stepFile(code == juce::KeyPress::rightKey ? 1 : -1);
        return true;
    }
    return false;
}

// A click anywhere gives the window the keyboard, unless a control inside already has it (a value being typed).
void RetroChipEditor::mouseDown(const juce::MouseEvent&)
{
    if (!hasKeyboardFocus(true))
        grabKeyboardFocus();
}

namespace
{
// The item `delta` places away in a menu, kept within it; with none chosen, the first or the last. `usable` leaves
// some out. -1 when there is nowhere to go.
int steppedIndex(int current, int count, int delta, const std::function<bool(int)>& usable)
{
    const int start = current >= 0 ? current : delta > 0 ? -1 : count;
    for (int next = start + delta; next >= 0 && next < count; next += delta)
        if (usable == nullptr || usable(next))
            return next;
    return -1;
}
} // namespace

// A timbre of the user's that has nothing imported would be silent, so the arrows pass it by.
bool RetroChipEditor::hasTimbre(int index) const
{
    if (index == GenesisEngine::kUserPatch)
        return processorRef.getUserPatch().has_value();
    if (index == GenesisEngine::kPcmPatch)
        return processorRef.getUserPcmName().isNotEmpty();
    if (index == GenesisEngine::kPcmKitPatch)
        return processorRef.getKitDescription().isNotEmpty();
    return true;
}

void RetroChipEditor::stepSound(int delta)
{
    if (shownConsole == Console::SoundFont)
    {
        const int next = steppedIndex(soundFontInstrumentBox.getSelectedItemIndex(), soundFontInstrumentBox.getNumItems(), delta, nullptr);
        if (next >= 0)
            soundFontInstrumentBox.setSelectedItemIndex(next, juce::sendNotificationSync);
    }
    else if (shownConsole == Console::Ym2612)
    {
        const int next = steppedIndex(timbreIndex, timbreBox.getNumItems(), delta, [this](int i) { return hasTimbre(i); });
        if (next >= 0)
        {
            timbreBox.setSelectedItemIndex(next, juce::dontSendNotification);
            selectTimbre();
        }
    }
    else if (shownConsole == Console::SDsp)
    {
        const auto usable = [this](int i) { return i != SnesEngine::kUserInstrument || processorRef.getUserSampleName().isNotEmpty(); };
        const int next = steppedIndex(instrumentIndex, instrumentBox.getNumItems(), delta, usable);
        if (next >= 0)
            instrumentAttachment->setValueAsCompleteGesture(static_cast<float>(next));
    }
    else
    {
        stepPreset(delta);
    }
}

void RetroChipEditor::stepFile(int delta)
{
    if (shownConsole == Console::SoundFont)
    {
        const int next = steppedIndex(soundFontBox.getSelectedId() - 1, static_cast<int>(libraryEntries.size()), delta, nullptr);
        if (next >= 0)
            soundFontBox.setSelectedId(next + 1, juce::sendNotificationSync);
    }
    else if (shownConsole == Console::Ym2612)
    {
        genLibrary.step(delta);
    }
    else if (shownConsole == Console::Chip2A03 || shownConsole == Console::Vrc6 || shownConsole == Console::N163)
    {
        nesLibrary.step(delta);
    }
    else
    {
        stepPreset(delta);
    }
}

// The presets of the console in view, in the menu's order; from the current one if it is the console's.
void RetroChipEditor::stepPreset(int delta)
{
    std::vector<int> programs;
    for (int i = 0; i < processorRef.getNumPrograms(); ++i)
        if (kPresets[static_cast<size_t>(i)].console == static_cast<int>(shownConsole))
            programs.push_back(i);
    const auto at = std::find(programs.begin(), programs.end(), processorRef.getCurrentProgram());
    const int current = at == programs.end() ? -1 : static_cast<int>(at - programs.begin());
    const int next = steppedIndex(current, static_cast<int>(programs.size()), delta, nullptr);
    if (next >= 0)
        presetBox.setSelectedId(programs[static_cast<size_t>(next)] + 1, juce::sendNotificationSync);
}

void RetroChipEditor::languageChanged()
{
    applyLanguage();
}

// The synth's own terms (Attack, Pulse, the panel names) are the same in both languages, so what changes is the
// buttons, the tooltips, the menus of the user's sounds and what the statuses say. A message that came before the
// change stays in its language until the next one.
void RetroChipEditor::applyLanguage()
{
    consoleControl.applyLanguage();

    for (auto* button : { &importButton, &genImportButton, &nesImportButton })
        button->setButtonText(tr("Import"));
    nesImportButton.setTooltip(tr("An .fti file: a FamiTracker instrument. A WAV: a sample for the DPCM channel."));
    genImportButton.setTooltip(tr("A .tfi file: an FM timbre. A WAV: one sound across the keyboard. Several WAVs: a kit, one sound per key."));
    nesInstrumentToggle.setTooltip(tr("Plays the .fti instrument imported for this chip instead of the envelope"));

    soundFontBox.setTextWhenNothingSelected(tr("Choose a file"));
    openFolderButton.setButtonText(tr("Open folder"));
    openFolderButton.setTooltip(tr("Opens the SoundFont folder, and creates it if it does not exist"));
    refreshButton.setButtonText(tr("Refresh"));
    refreshButton.setTooltip(tr("Reads the folder again to find the files added"));

    fillChoices(timbreBox, *processorRef.apvts.getParameter(ids::genPatch), timbreIndex);
    fillChoices(instrumentBox, *processorRef.apvts.getParameter(ids::snesInstrument), instrumentIndex);
    snesSustainRateKnob.refreshText();

    // A folder that is not in view is read when it comes into view.
    if (genLibraryShown)
        genLibrary.applyLanguage();
    if (nesLibraryShown)
        nesLibrary.applyLanguage();
    if (soundFontShown)
        listLibrary();

    englishButton.setToggleState(currentLanguage() == Language::english, juce::dontSendNotification);
    frenchButton.setToggleState(currentLanguage() == Language::french, juce::dontSendNotification);

    refreshSampleStatus();
    refreshSoundFontPanel();
    repaint();
}

// The menu shows the current preset only on that preset's console. Elsewhere it shows nothing, which also
// means the preset can be picked again: a combo box says nothing when its item is chosen a second time.
void RetroChipEditor::refreshPresetBox()
{
    const int program = processorRef.getCurrentProgram();
    const bool onItsConsole = kPresets[static_cast<size_t>(program)].console == static_cast<int>(shownConsole);
    const int id = onItsConsole ? program + 1 : 0;
    if (presetBox.getSelectedId() != id)
        presetBox.setSelectedId(id, juce::dontSendNotification);
}

void RetroChipEditor::refreshSampleStatus()
{
    const auto status = processorRef.getSampleStatus();
    if (sampleStatusLabel.getText() != status)
        sampleStatusLabel.setText(status, juce::dontSendNotification);
    sampleStatusLabel.setTooltip(status);

    const auto genesisStatus = processorRef.getGenesisStatus();
    if (genStatusLabel.getText() != genesisStatus)
        genStatusLabel.setText(genesisStatus, juce::dontSendNotification);
    const auto kit = processorRef.getKitDescription();
    genStatusLabel.setTooltip(timbreIndex == GenesisEngine::kPcmKitPatch && kit.isNotEmpty() ? genesisStatus + "\n" + kit : genesisStatus);

    const auto nesStatus = processorRef.getNesStatus();
    if (nesStatusLabel.getText() != nesStatus)
        nesStatusLabel.setText(nesStatus, juce::dontSendNotification);
    const auto nesKit = processorRef.getNesKitDescription();
    nesStatusLabel.setTooltip(shownConsole == Console::Chip2A03 && channelIndex == 3 && nesKit.isNotEmpty() ? nesStatus + "\n" + nesKit : nesStatus);
}

// The dialog is asynchronous: the editor goes on, and the import runs when a file is chosen. It is the
// message thread's work either way.
void RetroChipEditor::chooseSample()
{
    chooseFiles(tr("Import a WAV file"), "*.wav;*.wave", 0,
                [this](const juce::Array<juce::File>& files) { importFile(files.getFirst()); });
}

void RetroChipEditor::chooseFiles(const juce::String& title, const juce::String& patterns, int extraFlags, std::function<void(const juce::Array<juce::File>&)> onChosen)
{
    // A chooser that has finished is replaced here, not from its own callback.
    fileChooser = std::make_unique<juce::FileChooser>(title, lastImportFolder, patterns);
    juce::Component::SafePointer<RetroChipEditor> self(this);
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | extraFlags,
                             [self, callback = std::move(onChosen)](const juce::FileChooser& chooser)
                             {
                                 if (self == nullptr)
                                     return;
                                 const auto files = chooser.getResults();
                                 if (!files.isEmpty() && files.getFirst() != juce::File())
                                 {
                                     self->lastImportFolder = files.getFirst().getParentDirectory();
                                     callback(files);
                                 }
                             });
}

void RetroChipEditor::importFile(const juce::File& file)
{
    bool imported = false;
    processorRef.importSample(file, &imported);
    if (imported)
        instrumentAttachment->setValueAsCompleteGesture(static_cast<float>(SnesEngine::kUserInstrument));
}

void RetroChipEditor::importGenesisFile(const juce::File& file)
{
    juce::Array<juce::File> files;
    files.add(file);
    importGenesisFiles(files);
}

void RetroChipEditor::importGenesisFiles(const juce::Array<juce::File>& files)
{
    if (files.isEmpty())
        return;

    int timbre = -1;
    if (files.size() > 1 || (timbreIndex == GenesisEngine::kPcmKitPatch && !files.getFirst().hasFileExtension("tfi")))
    {
        bool imported = false;
        processorRef.importGenesisKit(files, &imported);
        if (imported)
            timbre = GenesisEngine::kPcmKitPatch;
    }
    else
    {
        auto imported = GenesisImport::none;
        processorRef.importGenesisFile(files.getFirst(), &imported);
        if (imported != GenesisImport::none)
            timbre = imported == GenesisImport::patch ? GenesisEngine::kUserPatch : GenesisEngine::kPcmPatch;
    }

    if (timbre >= 0)
    {
        timbreBox.setSelectedItemIndex(timbre, juce::dontSendNotification);
        selectTimbre();
    }
}

// The NES's chips are consoles of the parameter: 0 is the 2A03, 5 the VRC6, 6 the N163.
int RetroChipEditor::nesChip() const
{
    return shownConsole == Console::Vrc6 ? 1 : shownConsole == Console::N163 ? 2 : 0;
}

void RetroChipEditor::importNesFile(const juce::File& file)
{
    auto imported = NesImport::none;
    int chip = 0;
    processorRef.importNesFile(file, &imported, &chip);
    if (imported == NesImport::none)
        return;

    auto& apvts = processorRef.apvts;
    if (imported == NesImport::dpcm)
    {
        setParameterValue(*apvts.getParameter(ids::console), 0.0f);
        setParameterValue(*apvts.getParameter(ids::channel), 3.0f);
        setParameterValue(*apvts.getParameter(ids::nesUseInstrument), 0.0f);
        return;
    }

    constexpr float consoles[] = { 0.0f, 5.0f, 6.0f };
    setParameterValue(*apvts.getParameter(ids::console), consoles[juce::jlimit(0, 2, chip)]);
    setParameterValue(*apvts.getParameter(ids::nesUseInstrument), 1.0f);
    if (chip == 0 && processorRef.isNesKitOnly())
        setParameterValue(*apvts.getParameter(ids::channel), 3.0f);
}

void RetroChipEditor::timerCallback()
{
    processorRef.releaseRetiredSamples(); // frees what the audio thread has moved on from

    // A file that arrived, an instrument that changed, a message: the processor says so with its revision.
    // The processor frees its own retired banks, so nothing of that is done here.
    if (processorRef.getSoundFontRevision() != seenSoundFontRevision)
    {
        folderMessage.clear();
        refreshSoundFontPanel();
    }

    // The host can change the program without going through this editor. Only a real change is shown, since
    // the menu's own pick reaches the processor a moment after the box changes and must not be undone here.
    const int program = processorRef.getCurrentProgram();
    if (program != seenProgram)
    {
        seenProgram = program;
        refreshPresetBox();
    }
}

// Reads the folder: the file menu has a heading for each group, and its items are numbered by their place in the
// scan. This is the message thread's disk access, done when the user asks and when the console comes into view.
// A project that opened with its file missing tries it again here, since the user may have put it back.
void RetroChipEditor::rescanLibrary()
{
    processorRef.retrySoundFont();
    listLibrary();
}

void RetroChipEditor::listLibrary()
{
    libraryEntries = scanLibrary(processorRef.getLibraryRoot(), libraryMessage);
    folderMessage.clear();

    soundFontBox.clear(juce::dontSendNotification);
    juce::String group;
    for (size_t i = 0; i < libraryEntries.size(); ++i)
    {
        if (i == 0 || libraryEntries[i].group != group)
        {
            group = libraryEntries[i].group;
            soundFontBox.addSectionHeading(group);
        }
        soundFontBox.addItem(libraryEntries[i].name, static_cast<int>(i) + 1);
    }
    refreshSoundFontPanel();
}

// Shows what the processor has: the file in use, its instruments, the message. A long file name is cut by the
// menu, so the tooltip gives the path in full; an instrument's name the same.
void RetroChipEditor::refreshSoundFontPanel()
{
    seenSoundFontRevision = processorRef.getSoundFontRevision(); // before reading, so a change on the way is seen again

    const auto path = processorRef.getSoundFontPath();
    int id = 0;
    for (size_t i = 0; i < libraryEntries.size() && id == 0; ++i)
        if (libraryEntries[i].relativePath == path)
            id = static_cast<int>(i) + 1;
    if (soundFontBox.getSelectedId() != id)
        soundFontBox.setSelectedId(id, juce::dontSendNotification);
    soundFontBox.setTooltip(path);

    // The list is built again only when it has changed, which keeps an open menu as it is.
    const auto instruments = processorRef.getSoundFontInstruments();
    bool sameList = instruments.size() == listedInstruments.size();
    for (size_t i = 0; sameList && i < instruments.size(); ++i)
        sameList = instruments[i].name == listedInstruments[i].name && instruments[i].bank == listedInstruments[i].bank
                   && instruments[i].program == listedInstruments[i].program;
    if (!sameList)
    {
        listedInstruments = instruments;
        soundFontInstrumentBox.clear(juce::dontSendNotification);
        for (size_t i = 0; i < instruments.size(); ++i)
        {
            const auto& name = instruments[i].name;
            soundFontInstrumentBox.addItem(name.isNotEmpty() ? name : "Instrument " + juce::String(i + 1), static_cast<int>(i) + 1);
        }
    }
    const int index = processorRef.getSoundFontInstrumentIndex();
    const int instrumentId = index >= 0 && index < static_cast<int>(instruments.size()) ? index + 1 : 0;
    if (soundFontInstrumentBox.getSelectedId() != instrumentId)
        soundFontInstrumentBox.setSelectedId(instrumentId, juce::dontSendNotification);
    soundFontInstrumentBox.setTooltip(soundFontInstrumentBox.getText());

    // The editor's own word first, then the processor's, then the folder's.
    auto status = folderMessage;
    if (status.isEmpty())
        status = processorRef.getSoundFontStatus();
    if (status.isEmpty())
        status = libraryMessage;
    if (soundFontStatusLabel.getText() != status)
        soundFontStatusLabel.setText(status, juce::dontSendNotification);
    soundFontStatusLabel.setTooltip(status);
}

// From the menu the file is asked for with no instrument, so that the processor starts on the file's first.
void RetroChipEditor::chooseSoundFontFile()
{
    const int index = soundFontBox.getSelectedId() - 1;
    if (index >= 0 && index < static_cast<int>(libraryEntries.size()))
        processorRef.selectSoundFont(libraryEntries[static_cast<size_t>(index)].relativePath);
    refreshSoundFontPanel();
}

void RetroChipEditor::chooseSoundFontInstrument()
{
    const int index = soundFontInstrumentBox.getSelectedItemIndex();
    if (index >= 0)
        processorRef.selectSoundFontInstrument(index);
    refreshSoundFontPanel();
}

// The folder is made if it is not there, shown, and read again, so the message that invites to add files is up
// to date. If it cannot be made, the message says so.
void RetroChipEditor::openLibraryFolder()
{
    const auto root = processorRef.getLibraryRoot();
    const auto made = createLibraryFolder(root);
    if (made.wasOk())
        root.revealToUser();
    rescanLibrary();
    if (made.failed())
    {
        folderMessage = tr("Folder cannot be created: ") + root.getFileName();
        refreshSoundFontPanel();
    }
}

void RetroChipEditor::selectTimbre()
{
    const int index = timbreBox.getSelectedItemIndex();
    if (index < 0)
        return;

    timbreAttachment->setValueAsCompleteGesture(static_cast<float>(index));

    // A sample has neither, and an imported timbre has them only once it is there.
    const auto patch = index == GenesisEngine::kUserPatch ? processorRef.getUserPatch()
                       : index < static_cast<int>(std::size(kFmPatches)) ? std::optional<FmPatch>(kFmPatches[index])
                                                                         : std::nullopt;
    if (!patch)
        return;
    setParameterValue(*processorRef.apvts.getParameter(ids::genAlgorithm), static_cast<float>(patch->algorithm + 1));
    setParameterValue(*processorRef.apvts.getParameter(ids::genFeedback), static_cast<float>(patch->feedback));
}

// What the row beside the timbre holds. Called when the console or the timbre changes.
// What the NES's selector row holds: the 2A03's channel with its duty and noise mode, or the DPCM channel's root
// note and loop, or the VRC6's or the N163's own.
void RetroChipEditor::updateNesControls()
{
    const bool chip2A03 = shownConsole == Console::Chip2A03;
    const bool dpcm = chip2A03 && channelIndex == 3;
    channelControl.setVisible(chip2A03);
    dutyControl.setVisible(chip2A03 && !dpcm);
    noiseControl.setVisible(chip2A03 && !dpcm);
    dpcmRootSlider.setVisible(dpcm);
    dpcmLoopToggle.setVisible(dpcm);
    vrc6ChannelControl.setVisible(shownConsole == Console::Vrc6);
    vrc6DutyControl.setVisible(shownConsole == Console::Vrc6);
    n163WaveBox.setVisible(shownConsole == Console::N163);
    n163ChannelsControl.setVisible(shownConsole == Console::N163);
    repaint();
}

void RetroChipEditor::updateTimbreControls()
{
    const bool ym2612 = shownConsole == Console::Ym2612;
    const bool sample = timbreIndex >= GenesisEngine::kPcmPatch;
    algorithmControl.setVisible(ym2612 && !sample);
    genRootSlider.setVisible(ym2612 && sample);
    genLoopToggle.setVisible(ym2612 && sample);
}

// The five consoles each have their own controls and share vibrato, glide (not the SoundFont), velocity and gain.
void RetroChipEditor::showConsole(int consoleIndex)
{
    shownConsole = static_cast<Console>(consoleIndex);
    const bool chip2A03 = shownConsole == Console::Chip2A03;
    const bool ym2612 = shownConsole == Console::Ym2612;
    const bool sDsp = shownConsole == Console::SDsp;
    const bool soundFont = shownConsole == Console::SoundFont;
    const bool psg = shownConsole == Console::Psg;
    const bool nes = chip2A03 || shownConsole == Console::Vrc6 || shownConsole == Console::N163;

    for (juce::Component* control : std::initializer_list<juce::Component*> {
             &psgChannelControl, &psgNoiseTypeControl, &psgNoiseRateControl, &psgAttackKnob, &psgDecayKnob, &psgSustainKnob, &psgReleaseKnob })
        control->setVisible(psg);

    for (juce::Component* control : std::initializer_list<juce::Component*> {
             &attackKnob, &decayKnob, &sustainKnob, &releaseKnob, &nesImportButton, &nesStatusLabel, &nesInstrumentToggle, &nesLibrary })
        control->setVisible(nes);
    updateNesControls();

    for (juce::Component* control : std::initializer_list<juce::Component*> {
             &timbreBox, &genImportButton, &genStatusLabel, &genLibrary, &feedbackKnob, &brightnessKnob, &fmAttackKnob, &fmDecayKnob, &fmReleaseKnob })
        control->setVisible(ym2612);
    updateTimbreControls();

    for (juce::Component* control : std::initializer_list<juce::Component*> {
             &instrumentBox, &importButton, &sampleStatusLabel, &rootNoteSlider, &loopToggle, &snesAttackKnob, &snesDecayKnob, &snesSustainLevelKnob,
             &snesSustainRateKnob, &snesReleaseKnob, &echoMixKnob, &echoDelayKnob, &echoFeedbackKnob })
        control->setVisible(sDsp);

    for (juce::Component* control : std::initializer_list<juce::Component*> {
             &soundFontBox, &soundFontInstrumentBox, &openFolderButton, &refreshButton, &soundFontStatusLabel,
             &sfEchoMixKnob, &sfEchoDelayKnob, &sfEchoFeedbackKnob })
        control->setVisible(soundFont);

    // The SF2 has a vibrato and a glide of its own, so the shared ones are not offered here.
    for (juce::Component* control : std::initializer_list<juce::Component*> {
             &vibratoRateKnob, &vibratoDepthKnob, &glideKnob })
        control->setVisible(!soundFont);
    for (juce::Component* control : std::initializer_list<juce::Component*> { &gainKnob, &velocityToggle })
        control->setVisible(true);

    // The shared knobs are narrower on the S-DSP, which has twelve in a row.
    for (auto* knob : { &vibratoRateKnob, &vibratoDepthKnob, &glideKnob, &gainKnob })
        knob->setCompact(sDsp);
    for (auto* knob : { &snesAttackKnob, &snesDecayKnob, &snesSustainLevelKnob, &snesSustainRateKnob, &snesReleaseKnob,
                        &echoMixKnob, &echoDelayKnob, &echoFeedbackKnob, &sfEchoMixKnob, &sfEchoDelayKnob, &sfEchoFeedbackKnob })
        knob->setCompact(true);

    // A folder is read when its console comes into view, since files may have been added since.
    if (soundFont && !soundFontShown)
        rescanLibrary();
    soundFontShown = soundFont;
    if (ym2612 && !genLibraryShown)
        genLibrary.rescan();
    genLibraryShown = ym2612;
    if (nes && !nesLibraryShown)
        nesLibrary.rescan();
    nesLibraryShown = nes;

    updateEnabledControls();
    refreshPresetBox();
    resized();
    repaint();
}

// The greying rules belong to the 2A03's channels. The other consoles use every shared control. The S-DSP
// greys the root note and the loop unless the instrument is the user's sample.
void RetroChipEditor::updateEnabledControls()
{
    const bool chip2A03 = shownConsole == Console::Chip2A03;

    // A sample plays without the chip's operators. The release macro still sets its fade, except in a kit, where
    // each sample plays as it is.
    const bool fmSound = timbreIndex < GenesisEngine::kPcmPatch;
    const bool kit = shownConsole == Console::Ym2612 && timbreIndex == GenesisEngine::kPcmKitPatch;
    for (auto* knob : { &feedbackKnob, &brightnessKnob, &fmAttackKnob, &fmDecayKnob })
        knob->setEnabled(fmSound);
    fmReleaseKnob.setEnabled(!kit);
    genRootSlider.setEnabled(!kit);
    genRootSlider.setAlpha(kit ? 0.35f : 1.0f);
    genLoopToggle.setEnabled(!kit);
    genLoopToggle.setAlpha(kit ? 0.35f : 1.0f);

    const bool userSample = instrumentIndex == SnesEngine::kUserInstrument;
    rootNoteSlider.setEnabled(userSample);
    rootNoteSlider.setAlpha(userSample ? 1.0f : 0.35f);
    loopToggle.setEnabled(userSample);
    loopToggle.setAlpha(userSample ? 1.0f : 0.35f);

    const bool pulse = channelIndex == 0;
    const bool triangle = chip2A03 && channelIndex == 1;
    const bool noise = chip2A03 && channelIndex == 2;
    const bool dpcm = chip2A03 && channelIndex == 3;

    // The NES's imported instrument, once there is one for the chip, takes the envelope's place. The DPCM channel has
    // neither volume nor envelope.
    const bool hasInstrument = processorRef.hasNesInstrument(nesChip());
    nesInstrumentToggle.setEnabled(hasInstrument);
    nesInstrumentToggle.setAlpha(hasInstrument ? 1.0f : 0.35f);
    const bool instrumentPlays = hasInstrument && nesInstrumentOn && !dpcm;

    // The SN76489's noise settings are only for noise, and at a fixed rate it has no pitch to move.
    const bool psgNoise = psgChannelIndex == 1;
    psgNoiseTypeControl.setEnabled(psgNoise);
    psgNoiseRateControl.setEnabled(psgNoise);
    const bool psgFixedRate = shownConsole == Console::Psg && psgNoise && psgRateIndex != PsgEngine::kNoiseFollowsNote;

    dutyControl.setEnabled(pulse);
    noiseControl.setEnabled(channelIndex == 2);

    for (auto* knob : { &attackKnob, &decayKnob, &sustainKnob, &releaseKnob })
        knob->setEnabled(!triangle && !dpcm && !instrumentPlays);

    velocityToggle.setEnabled(!triangle && !dpcm);
    velocityToggle.setAlpha(triangle || dpcm ? 0.35f : 1.0f);

    for (auto* knob : { &vibratoRateKnob, &vibratoDepthKnob, &glideKnob })
        knob->setEnabled(!noise && !kit && !psgFixedRate && !dpcm);
}

void RetroChipEditor::paint(juce::Graphics& g)
{
    g.fillAll(palette::background);

    g.setColour(palette::accent);
    g.fillRect(kMargin, 18, 8, 22);
    g.setColour(palette::text);
    g.setFont(RetroLookAndFeel::retroFont(24.0f, true));
    g.drawText("RETROCHIP", kMargin + 16, 12, 170, 34, juce::Justification::centredLeft);

    // The legal notice the AGPLv3 asks an interactive program to show.
    g.setColour(palette::dimText);
    g.setFont(RetroLookAndFeel::retroFont(10.0f));
    g.drawText(tr("\xc2\xa9 2026 SleeperMusic \xc2\xb7 GNU AGPLv3 licence, without any warranty \xc2\xb7 Licences and source code: see the README"),
               juce::Rectangle<int>(kMargin, kHeight - kMargin, languageArea().getX() - 8 - kMargin, kMargin), juce::Justification::centred);

    if (shownConsole == Console::Ym2612)
    {
        auto selectors = selectorArea();
        drawPanel(g, takeTimbreArea(selectors), "TIMBRE");
        selectors.removeFromLeft(kGap);
        drawPanel(g, takeFmImportArea(selectors), "IMPORT");
        selectors.removeFromLeft(kGap);
        if (timbreIndex >= GenesisEngine::kPcmPatch)
        {
            drawPanel(g, takeFmRootArea(selectors), "ROOT NOTE");
            selectors.removeFromLeft(kGap);
            drawPanel(g, selectors, "LOOP");
        }
        else
        {
            drawPanel(g, selectors, "ALGORITHM");
        }

        auto knobs = knobArea();
        drawPanel(g, takeFmSoundArea(knobs), "SOUND");
        knobs.removeFromLeft(kGap);
        drawPanel(g, takeFmEnvelopeArea(knobs), "ENVELOPE");
        knobs.removeFromLeft(kGap);
        drawPanel(g, takeFmMotionArea(knobs), "VIBRATO + GLIDE");
        knobs.removeFromLeft(kGap);
        drawPanel(g, knobs, "OUTPUT");
        return;
    }

    if (shownConsole == Console::Psg)
    {
        auto selectors = selectorArea();
        drawPanel(g, takePsgChannelArea(selectors), "CHANNEL");
        selectors.removeFromLeft(kGap);
        drawPanel(g, takePsgNoiseTypeArea(selectors), "NOISE TYPE");
        selectors.removeFromLeft(kGap);
        drawPanel(g, selectors, "NOISE RATE");

        auto knobs = knobArea();
        drawPanel(g, takeEnvelopeArea(knobs), "ENVELOPE");
        knobs.removeFromLeft(kGap);
        drawPanel(g, takeMotionArea(knobs), "VIBRATO + GLIDE");
        knobs.removeFromLeft(kGap);
        drawPanel(g, knobs, "OUTPUT");
        return;
    }

    if (shownConsole == Console::SoundFont)
    {
        auto selectors = selectorArea();
        drawPanel(g, takeSfFileArea(selectors), tr("FILE"));
        selectors.removeFromLeft(kGap);
        drawPanel(g, takeSfInstrumentArea(selectors), "INSTRUMENT");
        selectors.removeFromLeft(kGap);
        drawPanel(g, takeSfFolderArea(selectors), tr("FOLDER"));
        selectors.removeFromLeft(kGap);
        drawPanel(g, selectors, tr("STATUS"));

        auto knobs = knobArea();
        drawPanel(g, takeSfEchoArea(knobs), "ECHO");
        knobs.removeFromLeft(kGap);
        drawPanel(g, takeSfOutputArea(knobs), "OUTPUT");
        return;
    }

    if (shownConsole == Console::SDsp)
    {
        auto selectors = selectorArea();
        drawPanel(g, takeSnesInstrumentArea(selectors), "INSTRUMENT");
        selectors.removeFromLeft(kGap);
        drawPanel(g, takeSnesImportArea(selectors), "IMPORT");
        selectors.removeFromLeft(kGap);
        drawPanel(g, takeSnesRootArea(selectors), "ROOT NOTE");
        selectors.removeFromLeft(kGap);
        drawPanel(g, selectors, "LOOP");

        auto knobs = knobArea();
        drawPanel(g, takeSnesEnvelopeArea(knobs), "ENVELOPE");
        knobs.removeFromLeft(kGap);
        drawPanel(g, takeSnesEchoArea(knobs), "ECHO");
        knobs.removeFromLeft(kGap);
        drawPanel(g, takeSnesMotionArea(knobs), "VIBRATO + GLIDE");
        knobs.removeFromLeft(kGap);
        drawPanel(g, knobs, "OUTPUT");
        return;
    }

    // The NES's three chips.
    auto selectors = selectorArea();
    if (shownConsole == Console::Vrc6)
    {
        drawPanel(g, takeVrc6ChannelArea(selectors), "CHANNEL");
        selectors.removeFromLeft(kGap);
        drawPanel(g, selectors, "PULSE WIDTH");
    }
    else if (shownConsole == Console::N163)
    {
        drawPanel(g, takeN163WaveArea(selectors), "WAVE");
        selectors.removeFromLeft(kGap);
        drawPanel(g, selectors, "CHANNELS");
    }
    else
    {
        const bool dpcm = channelIndex == 3;
        drawPanel(g, takeChannelArea(selectors), "CHANNEL");
        selectors.removeFromLeft(kGap);
        drawPanel(g, takeDutyArea(selectors), dpcm ? "ROOT NOTE" : "PULSE WIDTH");
        selectors.removeFromLeft(kGap);
        drawPanel(g, selectors, dpcm ? "LOOP" : "NOISE MODE");
    }

    auto knobs = knobArea();
    drawPanel(g, takeEnvelopeArea(knobs), "ENVELOPE");
    knobs.removeFromLeft(kGap);
    drawPanel(g, takeMotionArea(knobs), "VIBRATO + GLIDE");
    knobs.removeFromLeft(kGap);
    drawPanel(g, knobs, "OUTPUT");
}

void RetroChipEditor::resized()
{
    presetBox.setBounds(kWidth - kMargin - 176, 13, 176, 30);
    consoleControl.setBounds(kMargin + 170, 13, 372, 30); // three tabs, then the tab's two chips

    if (shownConsole == Console::Chip2A03 || shownConsole == Console::Vrc6 || shownConsole == Console::N163)
        layoutChip2A03();
    else if (shownConsole == Console::Ym2612)
        layoutYm2612();
    else if (shownConsole == Console::SDsp)
        layoutSDsp();
    else if (shownConsole == Console::Psg)
        layoutPsg();
    else
        layoutSoundFont();

    const auto keyboardArea = juce::Rectangle<int>(kMargin, kHeight - kMargin - kKeyboardHeight, kWidth - 2 * kMargin, kKeyboardHeight);
    keyboard.setBounds(keyboardArea);
    keyboard.setKeyWidth(static_cast<float>(keyboardArea.getWidth()) / 36.0f);

    auto language = languageArea();
    englishButton.setBounds(language.removeFromLeft(kLanguageButtonWidth));
    frenchButton.setBounds(language);
}

// The NES's three chips: each has its selector row, and they share the knobs.
void RetroChipEditor::layoutChip2A03()
{
    const auto selectorRow = [](juce::Rectangle<int> area)
    {
        return panelContent(area).withSizeKeepingCentre(area.getWidth() - 16, 36);
    };

    auto selectors = selectorArea();
    if (shownConsole == Console::Vrc6)
    {
        vrc6ChannelControl.setBounds(selectorRow(takeVrc6ChannelArea(selectors)));
        selectors.removeFromLeft(kGap);
        vrc6DutyControl.setBounds(selectorRow(selectors));
    }
    else if (shownConsole == Console::N163)
    {
        n163WaveBox.setBounds(selectorRow(takeN163WaveArea(selectors)));
        selectors.removeFromLeft(kGap);
        n163ChannelsControl.setBounds(selectorRow(selectors));
    }
    else
    {
        channelControl.setBounds(selectorRow(takeChannelArea(selectors)));
        selectors.removeFromLeft(kGap);
        const auto duty = selectorRow(takeDutyArea(selectors));
        dutyControl.setBounds(duty);
        dpcmRootSlider.setBounds(duty);
        selectors.removeFromLeft(kGap);
        noiseControl.setBounds(selectorRow(selectors));
        dpcmLoopToggle.setBounds(selectorRow(selectors));
    }

    layoutNesKnobs();
}

// The envelope panel has the import across its top: the button, the switch to the instrument, and the status.
void RetroChipEditor::layoutNesKnobs()
{
    auto knobs = knobArea();

    auto envelope = panelContent(takeEnvelopeArea(knobs));
    auto strip = envelope.removeFromTop(kNesImportStripHeight);
    nesImportButton.setBounds(strip.removeFromLeft(78));
    strip.removeFromLeft(6);
    nesInstrumentToggle.setBounds(strip.removeFromLeft(112));
    strip.removeFromLeft(4);
    nesStatusLabel.setBounds(strip);
    envelope.removeFromTop(4);
    nesLibrary.setBounds(envelope.removeFromTop(kNesLibraryHeight));

    const int envelopeWidth = envelope.getWidth() / 4;
    for (auto* knob : { &attackKnob, &decayKnob, &sustainKnob, &releaseKnob })
        knob->setBounds(envelope.removeFromLeft(envelopeWidth));

    knobs.removeFromLeft(kGap);
    auto motion = panelContent(takeMotionArea(knobs));
    const int motionWidth = motion.getWidth() / 3;
    for (auto* knob : { &vibratoRateKnob, &vibratoDepthKnob, &glideKnob })
        knob->setBounds(motion.removeFromLeft(motionWidth));

    knobs.removeFromLeft(kGap);
    auto output = panelContent(knobs);
    gainKnob.setBounds(output); // same height as the other panels, so the knobs line up
    velocityToggle.setBounds(output.removeFromBottom(26));
}

void RetroChipEditor::layoutPsg()
{
    const auto selectorRow = [](juce::Rectangle<int> area)
    {
        return panelContent(area).withSizeKeepingCentre(area.getWidth() - 16, 36);
    };

    auto selectors = selectorArea();
    psgChannelControl.setBounds(selectorRow(takePsgChannelArea(selectors)));
    selectors.removeFromLeft(kGap);
    psgNoiseTypeControl.setBounds(selectorRow(takePsgNoiseTypeArea(selectors)));
    selectors.removeFromLeft(kGap);
    psgNoiseRateControl.setBounds(selectorRow(selectors));

    auto knobs = knobArea();

    auto envelope = panelContent(takeEnvelopeArea(knobs));
    const int envelopeWidth = envelope.getWidth() / 4;
    for (auto* knob : { &psgAttackKnob, &psgDecayKnob, &psgSustainKnob, &psgReleaseKnob })
        knob->setBounds(envelope.removeFromLeft(envelopeWidth));

    knobs.removeFromLeft(kGap);
    auto motion = panelContent(takeMotionArea(knobs));
    const int motionWidth = motion.getWidth() / 3;
    for (auto* knob : { &vibratoRateKnob, &vibratoDepthKnob, &glideKnob })
        knob->setBounds(motion.removeFromLeft(motionWidth));

    knobs.removeFromLeft(kGap);
    auto output = panelContent(knobs);
    gainKnob.setBounds(output);
    velocityToggle.setBounds(output.removeFromBottom(26));
}

void RetroChipEditor::layoutYm2612()
{
    const auto selectorRow = [](juce::Rectangle<int> area)
    {
        return panelContent(area).withSizeKeepingCentre(area.getWidth() - 16, 36);
    };

    auto selectors = selectorArea();
    timbreBox.setBounds(selectorRow(takeTimbreArea(selectors)));
    selectors.removeFromLeft(kGap);
    auto import = panelContent(takeFmImportArea(selectors));
    auto importRow = import.removeFromTop(kImportRowHeight);
    genImportButton.setBounds(importRow.removeFromLeft(78));
    genStatusLabel.setBounds(importRow.withTrimmedLeft(4));
    genLibrary.setBounds(import.removeFromBottom(kImportRowHeight));
    selectors.removeFromLeft(kGap);
    algorithmControl.setBounds(selectorRow(selectors));
    genRootSlider.setBounds(selectorRow(takeFmRootArea(selectors)));
    selectors.removeFromLeft(kGap);
    genLoopToggle.setBounds(selectorRow(selectors));

    const auto placeKnobs = [](juce::Rectangle<int> area, std::initializer_list<Knob*> row)
    {
        auto content = panelContent(area);
        for (auto* knob : row)
            knob->setBounds(content.removeFromLeft(kFmKnobWidth));
    };

    auto knobs = knobArea();
    placeKnobs(takeFmSoundArea(knobs), { &feedbackKnob, &brightnessKnob });
    knobs.removeFromLeft(kGap);
    placeKnobs(takeFmEnvelopeArea(knobs), { &fmAttackKnob, &fmDecayKnob, &fmReleaseKnob });
    knobs.removeFromLeft(kGap);
    placeKnobs(takeFmMotionArea(knobs), { &vibratoRateKnob, &vibratoDepthKnob, &glideKnob });
    knobs.removeFromLeft(kGap);

    auto output = panelContent(knobs);
    gainKnob.setBounds(output);
    velocityToggle.setBounds(output.removeFromBottom(26));
}

void RetroChipEditor::layoutSDsp()
{
    const auto selectorRow = [](juce::Rectangle<int> area)
    {
        return panelContent(area).withSizeKeepingCentre(area.getWidth() - 16, 36);
    };

    auto selectors = selectorArea();
    instrumentBox.setBounds(selectorRow(takeSnesInstrumentArea(selectors)));
    selectors.removeFromLeft(kGap);
    auto import = selectorRow(takeSnesImportArea(selectors));
    importButton.setBounds(import.removeFromLeft(78));
    sampleStatusLabel.setBounds(import.withTrimmedLeft(4));
    selectors.removeFromLeft(kGap);
    rootNoteSlider.setBounds(selectorRow(takeSnesRootArea(selectors)));
    selectors.removeFromLeft(kGap);
    loopToggle.setBounds(selectorRow(selectors));

    const auto placeKnobs = [](juce::Rectangle<int> area, std::initializer_list<Knob*> row)
    {
        auto content = panelContent(area, kSnesPanelPadding);
        for (auto* knob : row)
            knob->setBounds(content.removeFromLeft(kSnesKnobWidth));
    };

    auto knobs = knobArea();
    placeKnobs(takeSnesEnvelopeArea(knobs), { &snesAttackKnob, &snesDecayKnob, &snesSustainLevelKnob, &snesSustainRateKnob, &snesReleaseKnob });
    knobs.removeFromLeft(kGap);
    placeKnobs(takeSnesEchoArea(knobs), { &echoMixKnob, &echoDelayKnob, &echoFeedbackKnob });
    knobs.removeFromLeft(kGap);
    placeKnobs(takeSnesMotionArea(knobs), { &vibratoRateKnob, &vibratoDepthKnob, &glideKnob });
    knobs.removeFromLeft(kGap);

    // The gain knob is a little wider than the others, for "-36.0 dB".
    auto output = panelContent(knobs, kSnesPanelPadding);
    gainKnob.setBounds(output.withSizeKeepingCentre(kSnesGainKnobWidth, output.getHeight()));
    velocityToggle.setBounds(output.removeFromBottom(26));
}

void RetroChipEditor::layoutSoundFont()
{
    const auto selectorRow = [](juce::Rectangle<int> area)
    {
        return panelContent(area).withSizeKeepingCentre(area.getWidth() - 16, 36);
    };

    auto selectors = selectorArea();
    soundFontBox.setBounds(selectorRow(takeSfFileArea(selectors)));
    selectors.removeFromLeft(kGap);
    soundFontInstrumentBox.setBounds(selectorRow(takeSfInstrumentArea(selectors)));
    selectors.removeFromLeft(kGap);

    auto folder = panelContent(takeSfFolderArea(selectors));
    openFolderButton.setBounds(folder.removeFromTop(kSfButtonHeight));
    refreshButton.setBounds(folder.removeFromBottom(kSfButtonHeight));
    selectors.removeFromLeft(kGap);
    soundFontStatusLabel.setBounds(panelContent(selectors));

    auto knobs = knobArea();
    auto echo = panelContent(takeSfEchoArea(knobs));
    for (auto* knob : { &sfEchoMixKnob, &sfEchoDelayKnob, &sfEchoFeedbackKnob })
        knob->setBounds(echo.removeFromLeft(kSfKnobWidth));
    knobs.removeFromLeft(kGap);

    auto output = panelContent(takeSfOutputArea(knobs));
    gainKnob.setBounds(output);
    velocityToggle.setBounds(output.removeFromBottom(26));
}
} // namespace retrochip
