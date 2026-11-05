// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "SoundFontLibrary.h"

#include "Localisation.h"

#include <algorithm>

namespace retrochip
{
namespace
{
constexpr int kMaxFolderDepth = 8;

const char* extensionOf(Library library)
{
    return library == Library::tfi ? "tfi" : library == Library::fti ? "fti" : "sf2";
}

juce::String text(const char* utf8) { return juce::String::fromUTF8(utf8); }

// For sorting only: a letter with its accent counts as that letter, composed or followed by a combining accent.
juce::String withoutAccents(const juce::String& name)
{
    static const juce::String accented = text("ÀÁÂÃÄÅÆàáâãäåæÇçÈÉÊËèéêëÌÍÎÏìíîïÑñÒÓÔÕÖØŒòóôõöøœÙÚÛÜùúûüÝŸýÿ");
    static const juce::String plain = "AAAAAAAaaaaaaaCcEEEEeeeeIIIIiiiiNnOOOOOOOoooooooUUUUuuuuYYyy";

    juce::String result;
    result.preallocateBytes(name.getNumBytesAsUTF8());
    for (auto p = name.getCharPointer(); !p.isEmpty();)
    {
        const auto c = p.getAndAdvance();
        if (c >= 0x300 && c <= 0x36F)
            continue;
        const int at = c >= 0x80 ? accented.indexOfChar(c) : -1;
        result += at >= 0 ? plain[at] : c;
    }
    return result;
}

int compareNames(const juce::String& a, const juce::String& b)
{
    if (const int order = withoutAccents(a).compareNatural(withoutAccents(b), false); order != 0)
        return order;
    return a.compare(b); // two spellings stay apart, and in a fixed order
}

struct Scan
{
    const char* extension;
    juce::String otherGroup;
    std::vector<LibraryEntry> entries;
    juce::String message;
};

void collect(const juce::File& folder, const juce::String& relativeFolder, const juce::String& group, int depth, Scan& scan)
{
    if (!folder.hasReadAccess())
    {
        if (scan.message.isEmpty())
            scan.message = tr("Unreadable folder: ") + folder.getFileName();
        return;
    }

    const auto children = folder.findChildFiles(juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles, false);
    for (const auto& child : children)
    {
        const auto name = child.getFileName();
        if (child.isDirectory())
        {
            if (depth < kMaxFolderDepth)
                collect(child, relativeFolder + name + "/", depth == 0 ? name : group, depth + 1, scan);
        }
        else if (child.hasFileExtension(scan.extension))
        {
            scan.entries.push_back({ depth == 0 ? scan.otherGroup : group, child.getFileNameWithoutExtension(), relativeFolder + name });
        }
    }
}
} // namespace

juce::File defaultLibraryRoot(Library library)
{
    const char* folder = library == Library::tfi ? "RetroChip/Genesis" : library == Library::fti ? "RetroChip/NES" : "RetroChip/SoundFonts";
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile(folder);
}

std::vector<LibraryEntry> scanLibrary(const juce::File& root, juce::String& message, Library library)
{
    Scan scan { extensionOf(library), tr("Other"), {}, {} };
    if (root.isDirectory())
        collect(root, {}, {}, 0, scan);

    auto& entries = scan.entries;
    const auto& other = scan.otherGroup;
    std::stable_sort(entries.begin(), entries.end(), [&other](const LibraryEntry& a, const LibraryEntry& b)
    {
        const bool aOther = a.group == other;
        if (aOther != (b.group == other))
            return b.group == other;
        if (const int order = compareNames(a.group, b.group); order != 0)
            return order < 0;
        if (const int order = compareNames(a.name, b.name); order != 0)
            return order < 0;
        return a.relativePath.compare(b.relativePath) < 0;
    });

    message = scan.message;
    if (entries.empty() && message.isEmpty())
        message = library == Library::tfi   ? tr("No timbre: add .tfi files to the folder")
                  : library == Library::fti ? tr("No instrument: add .fti files to the folder")
                                            : tr("No SoundFont: add .sf2 files to the folder");
    return entries;
}

juce::File resolveInLibrary(const juce::File& root, const juce::String& relativePath)
{
    if (root == juce::File() || relativePath.isEmpty())
        return {};
    if (relativePath.startsWithChar('/') || relativePath.startsWithChar('\\'))
        return {};

    juce::StringArray parts;
    parts.addTokens(relativePath, "/\\", "");
    if (parts[0] == "~" || parts.contains(".."))
        return {};
    for (const auto& part : parts)
        if (part.containsChar(':'))
            return {}; // a Windows drive, as in C:, would leave the folder there

    // Not getChildFile, which reads a first name like ~name as somebody's home folder.
    return juce::File(root.getFullPathName() + "/" + relativePath);
}
} // namespace retrochip
