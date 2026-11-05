// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

namespace retrochip
{
// What the plugin needs from any console's sound engine.
class ConsoleEngine
{
public:
    virtual ~ConsoleEngine() = default;

    virtual void prepare(double sampleRate) = 0;
    virtual void noteOn(int note, int velocity) = 0;
    virtual void noteOff(int note) = 0;
    virtual void setPitchBend(double semitones) = 0;
    virtual void setSustain(bool on) = 0;
    virtual void allNotesOff() = 0;
    virtual void render(float* out, int numSamples) = 0;
};
} // namespace retrochip
