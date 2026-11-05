// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace retrochip
{
// The user's folders: the SoundFonts, and the .tfi timbres and .fti instruments beside the Genesis's and the NES's
// import buttons.
enum class Library
{
    soundFonts,
    tfi,
    fti
};

// One file of a folder. `group` is the name of the first-level folder it is in, or "Other" (in the user's language)
// for a file at the root. `name` is the file's name without its extension. `relativePath` uses / and is what a saved
// state keeps.
struct LibraryEntry
{
    juce::String group;
    juce::String name;
    juce::String relativePath;
};

// ~/Music/RetroChip/SoundFonts, Genesis and NES. It is not created here; that is for the button that opens it.
juce::File defaultLibraryRoot(Library library = Library::soundFonts);

// The files of the folder with the library's extension, by group in alphabetical order with "Other" last, then by
// name. Folders may be
// symbolic links, so the search stops 8 folders down, which also ends a link that loops. Hidden files are left
// out. `message` says why the list is empty or short, and is empty when there is nothing to say. For the message
// thread: it reads the disk.
std::vector<LibraryEntry> scanLibrary(const juce::File& root, juce::String& message, Library library = Library::soundFonts);

// The file a relative path names in the folder, without checking that it exists. A null File if the path would
// leave the folder (a ".." anywhere, an absolute path, or a drive letter) or is empty: saved states come from outside.
juce::File resolveInLibrary(const juce::File& root, const juce::String& relativePath);
} // namespace retrochip
