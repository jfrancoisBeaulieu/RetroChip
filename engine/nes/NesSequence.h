// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include "Fti.h"

namespace retrochip
{
// Where a note is in one of its instrument's sequences, the way FamiTracker plays them. While the key is down the
// sequence plays to its release point and waits there, or goes round its loop when the loop comes before the release
// point. After release() it goes on from the step after the release point, and goes round a loop placed after it.
// With no loop the last value holds.
class SequenceCursor
{
public:
    void reset();

    // The value for this 60 Hz step, then moves on. False for a sequence that is off or empty.
    bool step(const FtiSequence& sequence, int& value);

    void release(const FtiSequence& sequence);

    // The last value is holding, with no loop to leave it.
    bool finished() const { return done; }

private:
    int index = 0;
    bool released = false;
    bool done = false;
};
} // namespace retrochip
