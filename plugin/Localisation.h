// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include <juce_core/juce_core.h>

#include <utility>
#include <vector>

namespace retrochip
{
enum class Language
{
    english,
    french
};

// The language is the user's, the same for every instance and every project: a settings file keeps it, and it is
// English until the user picks another. The file is read once, on the message thread, when the first processor is
// made; a file that cannot be read means English.
void loadLanguage();
Language currentLanguage();

// Saves the choice and tells every open editor. For the message thread.
void setLanguage(Language language);

// ~/Library/Application Support/SleeperMusic/RetroChip/Settings.xml on macOS, and SleeperMusic\RetroChip\Settings.xml
// in %APPDATA% on Windows. Tests give another, so that the user's own is never touched; that also reads it again.
juce::File settingsFile();
void useSettingsFile(const juce::File& file);

// Back to the state before the first load, with `file` as the settings: for a test of that first load.
void resetLanguageForTests(const juce::File& file);

// The text in the current language. `english` is the text and the key of the French table. Any thread may ask:
// the table is fixed once made and the language is atomic. A key the table does not have is kept, for the tests.
juce::String tr(const char* english);

// The French table, for the tests, and the keys asked for in French that it does not have.
const std::vector<std::pair<const char*, const char*>>& frenchTable();
juce::StringArray missingTranslations();

// A number with one decimal and the language's decimal mark: 3.6 or 3,6.
juce::String oneDecimal(double value);

// For the editors, which show the texts again when the language changes. Message thread only.
class LanguageListener
{
public:
    virtual ~LanguageListener() = default;
    virtual void languageChanged() = 0;
};

void addLanguageListener(LanguageListener* listener);
void removeLanguageListener(LanguageListener* listener);
} // namespace retrochip
