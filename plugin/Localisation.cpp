// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "Localisation.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace retrochip
{
namespace
{
constexpr const char* kSettingsTag = "RETROCHIP_SETTINGS";
constexpr const char* kLanguageAttribute = "language";

std::atomic<Language> language { Language::english };

std::mutex& fileMutex()
{
    static std::mutex mutex;
    return mutex;
}

juce::File& overriddenFile()
{
    static juce::File file;
    return file;
}

bool& loaded()
{
    static bool value = false;
    return value;
}

juce::ListenerList<LanguageListener>& listeners()
{
    static juce::ListenerList<LanguageListener> list;
    return list;
}

// ~/Library/Application Support on macOS, %APPDATA% on Windows, ~/.config on Linux.
juce::File defaultSettingsFile()
{
    const auto base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    return base.getChildFile("Application Support/SleeperMusic/RetroChip/Settings.xml");
   #else
    return base.getChildFile("SleeperMusic/RetroChip/Settings.xml");
   #endif
}

// The file in use. The caller holds fileMutex.
juce::File currentFile()
{
    return overriddenFile() != juce::File() ? overriddenFile() : defaultSettingsFile();
}

const char* codeOf(Language l) { return l == Language::french ? "fr" : "en"; }

void readFile(const juce::File& file)
{
    auto chosen = Language::english;
    if (const auto xml = juce::parseXMLIfTagMatches(file, kSettingsTag))
        if (xml->getStringAttribute(kLanguageAttribute) == codeOf(Language::french))
            chosen = Language::french;
    language = chosen;
}

const std::unordered_map<std::string, const char*>& frenchMap()
{
    static const auto map = []
    {
        std::unordered_map<std::string, const char*> result;
        for (const auto& [english, french] : frenchTable())
            result.emplace(english, french);
        return result;
    }();
    return map;
}

std::mutex& missingMutex()
{
    static std::mutex mutex;
    return mutex;
}

juce::StringArray& missing()
{
    static juce::StringArray keys;
    return keys;
}
} // namespace

const std::vector<std::pair<const char*, const char*>>& frenchTable()
{
    // The interface's own words. A synth's terms (Attack, Pulse, Gain, the panel names) are the same in both.
    static const std::vector<std::pair<const char*, const char*>> table {
        // The selector.
        { "The NES's 2A03 chip, and the VRC6 and Namco 163 chips of Famicom cartridges",
          "Puce 2A03 de la NES, et les puces VRC6 et Namco 163 des cartouches Famicom" },
        { "Two chips: YM2612 (FM) and SN76489 (PSG)", "Deux puces : YM2612 (FM) et SN76489 (PSG)" },
        { "The SNES's S-DSP chip, or SoundFont files", "Puce S-DSP de la SNES, ou fichiers SoundFont" },
        { "The NES's 2A03 chip: pulse, triangle, noise and DPCM", "Puce 2A03 de la NES : pulse, triangle, bruit et DPCM" },
        { "The Genesis's YM2612 chip: FM synthesis, TFI timbres, PCM", "Puce YM2612 de la Genesis : synthèse FM, timbres TFI, PCM" },
        { "The SNES's S-DSP chip", "Puce S-DSP de la SNES" },
        { "Player for .sf2 files", "Lecteur de fichiers .sf2" },
        { "The Genesis's SN76489 chip: square waves and noise", "Puce SN76489 de la Genesis : ondes carrées et bruit" },
        { "Konami's VRC6 chip, in Famicom cartridges: two pulses and a sawtooth",
          "Puce VRC6 de Konami, dans des cartouches Famicom : deux pulses et une dent de scie" },
        { "The Namco 163 chip, in Famicom cartridges: drawn waveforms",
          "Puce Namco 163, dans des cartouches Famicom : formes d'onde dessinées" },

        // Buttons, menus and their tooltips.
        { "Import", "Importer" },
        { "Import a WAV file", "Importer un fichier WAV" },
        { "Plays the .fti instrument imported for this chip instead of the envelope",
          "Joue l'instrument .fti importé pour cette puce au lieu de l'enveloppe" },
        { "An .fti file: a FamiTracker instrument. A WAV: a sample for the DPCM channel.",
          "Un fichier .fti : un instrument FamiTracker. Un WAV : un échantillon pour le canal DPCM." },
        { "Import an .fti instrument or a WAV", "Importer un instrument .fti ou un WAV" },
        { "A .tfi file: an FM timbre. A WAV: one sound across the keyboard. Several WAVs: a kit, one sound per key.",
          "Un fichier .tfi : un timbre FM. Un WAV : un son sur tout le clavier. Plusieurs WAV : un kit, un son par touche." },
        { "Import a TFI, a WAV or several WAVs", "Importer un TFI, un WAV ou plusieurs WAV" },
        { "Choose a file", "Choisir un fichier" },
        { "Open folder", "Ouvrir le dossier" },
        { "Refresh", "Actualiser" },
        { "Opens the SoundFont folder, and creates it if it does not exist",
          "Ouvre le dossier des SoundFonts, et le crée s'il n'existe pas" },
        { "Opens the folder of .tfi timbres, and creates it if it does not exist",
          "Ouvre le dossier des timbres .tfi, et le crée s'il n'existe pas" },
        { "Opens the folder of .fti instruments, and creates it if it does not exist",
          "Ouvre le dossier des instruments .fti, et le crée s'il n'existe pas" },
        { "Reads the folder again to find the files added", "Relit le dossier pour y trouver les fichiers ajoutés" },
        { "From the folder", "Depuis le dossier" },
        { "Empty folder", "Dossier vide" },
        { "Folder: ", "Dossier : " },
        { "Folder cannot be created", "Dossier impossible à créer" },
        { "Folder cannot be created: ", "Dossier impossible à créer : " },
        { "FILE", "FICHIER" },
        { "FOLDER", "DOSSIER" },
        { "STATUS", "ÉTAT" },
        { "\xc2\xa9 2026 SleeperMusic \xc2\xb7 GNU AGPLv3 licence, without any warranty \xc2\xb7 Licences and source code: see the README",
          "\xc2\xa9 2026 SleeperMusic \xc2\xb7 Licence GNU AGPLv3, sans aucune garantie \xc2\xb7 Licences et code source : voir le README" },

        // Parameter values.
        { "My TFI timbre", "Mon timbre TFI" },
        { "My PCM sample", "Mon échantillon PCM" },
        { "My PCM kit", "Mon kit PCM" },
        { "My sample", "Mon échantillon" },
        { "Infinite", "Infini" },
        { "Strings", "Cordes" },
        { "Brass", "Cuivres" },
        { "Organ", "Orgue" },
        { "Bass", "Basse" },
        { "Bell", "Cloche" },
        { "Flute", "Flûte" },
        { "Choir", "Chœur" },
        { "Piano", "Piano" },
        { "Guitar", "Guitare" },
        { "Marimba", "Marimba" },
        { "Reed", "Anche" },
        { "Pad", "Nappe" },

        // The folders.
        { "Other", "Autres" },
        { "Unreadable folder: ", "Dossier illisible : " },
        { "No SoundFont: add .sf2 files to the folder", "Aucun SoundFont : ajoute des fichiers .sf2 dans le dossier" },
        { "No timbre: add .tfi files to the folder", "Aucun timbre : ajoute des fichiers .tfi dans le dossier" },
        { "No instrument: add .fti files to the folder", "Aucun instrument : ajoute des fichiers .fti dans le dossier" },

        // What the imports say.
        { "File not found: ", "Fichier introuvable : " },
        { "Unreadable file: ", "Fichier illisible : " },
        { "Empty file: ", "Fichier vide : " },
        { "File too large (1 GB limit): ", "Fichier trop volumineux (limite de 1 Go) : " },
        { "Unsupported format: ", "Format non pris en charge : " },
        { "Unreadable sample in the saved state: import it again", "Échantillon illisible dans l'état : importe-le à nouveau" },
        { "Unreadable imported sound in the saved state: import it again", "Son importé illisible dans l'état : importe-le à nouveau" },
        { "Imported sample", "Échantillon importé" },
        { "Import a WAV file to play this sound", "Importe un fichier WAV pour jouer ce son" },
        { "Import a .tfi file to play this timbre", "Importe un fichier .tfi pour jouer ce timbre" },
        { "Import several WAV files to play this kit", "Importe plusieurs fichiers WAV pour jouer ce kit" },
        { "Import a WAV or an .fti with samples", "Importe un WAV ou un .fti avec des échantillons" },
        { "Import an .fti file for a game instrument", "Importe un fichier .fti pour un instrument de jeu" },
        { "Kit: ", "Kit : " },
        { " sounds", " sons" },
        { " sound", " son" },
        { ": ", " : " },
        { "Instrument not found (", "Instrument introuvable (" },
        { "): first instrument used", ") : premier instrument utilisé" },
        { "Instrument not found: first instrument used", "Instrument introuvable : premier instrument utilisé" },
        { "Sample cut at ", "Échantillon coupé à " },
        { "\xc2\xa0s (64 KB limit)", "\xc2\xa0s (limite de 64 Ko)" },
        { "\xc2\xa0s (256 KB limit)", "\xc2\xa0s (limite de 256 Ko)" },
        { "\xc2\xa0s (DPCM limit: 4081 bytes)", "\xc2\xa0s (limite du DPCM : 4081 octets)" },
        { "Invalid TFI file: ", "Fichier TFI invalide : " },
        { "Skipped: ", "Ignorés : " },
        { "Kit cut (256 KB limit)", "Kit coupé (limite de 256 Ko)" },
        { "Kit limited to ", "Kit limité à " },
        { "Unsupported chip (VRC7, FDS or 5B): ", "Puce non prise en charge (VRC7, FDS ou 5B) : " },
        { "Damaged FTI file: ", "Fichier FTI abîmé : " },
        { "Invalid FTI file: ", "Fichier FTI invalide : " },
    };
    return table;
}

void loadLanguage()
{
    const std::lock_guard lock(fileMutex());
    if (loaded())
        return;
    loaded() = true;
    readFile(currentFile());
}

Language currentLanguage()
{
    return language.load();
}

juce::File settingsFile()
{
    const std::lock_guard lock(fileMutex());
    return currentFile();
}

void useSettingsFile(const juce::File& file)
{
    const std::lock_guard lock(fileMutex());
    overriddenFile() = file;
    loaded() = true;
    readFile(file);
}

void resetLanguageForTests(const juce::File& file)
{
    const std::lock_guard lock(fileMutex());
    overriddenFile() = file;
    loaded() = false;
    language = Language::english;
}

void setLanguage(Language chosen)
{
    language = chosen;

    // The other settings of the file, if a later version adds some, are kept.
    const auto file = settingsFile();
    auto xml = juce::parseXMLIfTagMatches(file, kSettingsTag);
    if (xml == nullptr)
        xml = std::make_unique<juce::XmlElement>(kSettingsTag);
    xml->setAttribute(kLanguageAttribute, codeOf(chosen));
    if (file.getParentDirectory().createDirectory())
        xml->writeTo(file);

    listeners().call([](LanguageListener& l) { l.languageChanged(); });
}

juce::String tr(const char* english)
{
    if (language.load() == Language::english)
        return juce::String::fromUTF8(english);

    const auto& map = frenchMap();
    if (const auto found = map.find(english); found != map.end())
        return juce::String::fromUTF8(found->second);

    const std::lock_guard lock(missingMutex());
    missing().addIfNotAlreadyThere(juce::String::fromUTF8(english));
    return juce::String::fromUTF8(english);
}

juce::StringArray missingTranslations()
{
    const std::lock_guard lock(missingMutex());
    return missing();
}

juce::String oneDecimal(double value)
{
    const auto text = juce::String(value, 1);
    return currentLanguage() == Language::french ? text.replace(".", ",") : text;
}

void addLanguageListener(LanguageListener* listener)
{
    listeners().add(listener);
}

void removeLanguageListener(LanguageListener* listener)
{
    listeners().remove(listener);
}
} // namespace retrochip
