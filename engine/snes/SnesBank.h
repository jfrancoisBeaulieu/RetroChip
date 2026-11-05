// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include "Brr.h"

#include <memory>
#include <vector>

namespace retrochip
{
// The 12 instruments, in the order of the spec: Strings, Brass, Organ, Bass, Bell, Flute, Choir,
// Piano, Guitar, Marimba, Reed, Pad. The plugin shows them in the user's language.
//
// Each is built by additive synthesis from fixed seeds, without any global generator, so every
// call returns the same bytes. Each has a root note, which plays it at the chip's 32 kHz as it is.
//
// The sustained ones have a short run-up and then a loop at a block boundary, which holds a whole
// number of cycles of every partial: looping neither clicks nor detunes, and the root plays at its
// nominal pitch to a fraction of a cent. The decaying ones (Bell, Piano, Guitar, Marimba) have no
// loop and fade to silence, where the voice stops. Each is under 64 KB.
//
// Takes some time and allocates: build it once when the plugin is made, never on the audio thread.
std::vector<std::shared_ptr<const SnesSample>> buildSnesBank();
} // namespace retrochip
