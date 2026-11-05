// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#include "NesSequence.h"

#include <algorithm>

namespace retrochip
{
void SequenceCursor::reset()
{
    index = 0;
    released = false;
    done = false;
}

bool SequenceCursor::step(const FtiSequence& sequence, int& value)
{
    const int count = static_cast<int>(sequence.values.size());
    if (!sequence.enabled || count == 0)
        return false;

    index = std::clamp(index, 0, count - 1);
    value = sequence.values[static_cast<std::size_t>(index)];

    int next = index + 1;
    if (!released && sequence.release >= 0 && next > sequence.release)
    {
        // Held: round the loop if it comes first, else wait on the release point.
        next = sequence.loop >= 0 && sequence.loop <= sequence.release ? sequence.loop : sequence.release;
    }
    else if (next >= count)
    {
        const bool loops = sequence.loop >= 0 && (!released || sequence.release < 0 || sequence.loop > sequence.release);
        next = loops ? sequence.loop : count - 1;
        done = !loops;
    }
    index = next;
    return true;
}

void SequenceCursor::release(const FtiSequence& sequence)
{
    released = true;
    if (sequence.release < 0)
        return;
    index = sequence.release + 1;
    const int count = static_cast<int>(sequence.values.size());
    if (index >= count)
    {
        index = std::max(count - 1, 0);
        done = true;
    }
}
} // namespace retrochip
