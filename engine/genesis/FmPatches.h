// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

namespace retrochip
{
// Values in the YM2612's own units, written straight to its registers:
// mul 0 to 15 (0 means x 0.5), dt 0 to 7 (register coding: 1 to 3 sharp, 5 to 7 flat),
// tl 0 to 127 (attenuation in 0.75 dB steps), rs 0 to 3, ar, dr, sr 0 to 31 (higher is faster),
// sl 0 to 15 (3 dB steps below the peak), rr 0 to 15 (higher is faster), ssg 0 (off) or 8 to 15.
struct FmOperator
{
    int mul, dt, tl, rs, ar, dr, sr, sl, rr;
    int ssg = 0;
};

struct FmPatch
{
    const char* name;
    int algorithm, feedback; // 0 to 7 each; feedback acts on operator 1
    FmOperator ops[4];       // operators 1 to 4
};

// The chip's algorithms, with -> meaning "modulates":
//   0: 1->2->3->4          1: (1+2)->3->4        2: (1+(2->3))->4      3: ((1->2)+3)->4
//   4: (1->2)+(3->4)       5: 1->(2+3+4)         6: (1->2)+3+4         7: 1+2+3+4
// Original patches written for this project. Levels and decay times are measured by the test
// genesis_patches_level_and_decay.
inline constexpr FmPatch kFmPatches[12] = {
    { "Pure Tone", 7, 0, { { 1, 0, 127, 0, 31, 0, 0, 0, 10 },
                           { 1, 0, 127, 0, 31, 0, 0, 0, 10 },
                           { 1, 0, 127, 0, 31, 0, 0, 0, 10 },
                           { 1, 0, 0, 0, 31, 0, 0, 0, 10 } } },
    { "FM Bass", 3, 4, { { 1, 0, 28, 1, 31, 8, 2, 2, 9 },
                         { 1, 0, 34, 1, 31, 9, 2, 3, 9 },
                         { 1, 0, 46, 1, 31, 9, 2, 3, 9 },
                         { 1, 0, 0, 1, 31, 5, 2, 2, 10 } } },
    { "Slap Bass", 4, 5, { { 3, 0, 30, 1, 31, 14, 8, 6, 10 },
                           { 1, 0, 8, 1, 31, 10, 5, 3, 10 },
                           { 1, 3, 30, 1, 31, 15, 5, 6, 10 },
                           { 1, 0, 6, 1, 31, 8, 5, 3, 10 } } },
    { "E-Piano", 4, 1, { { 14, 0, 56, 2, 31, 15, 5, 15, 8 },
                         { 1, 2, 8, 2, 31, 9, 6, 4, 8 },
                         { 1, 6, 42, 2, 31, 8, 3, 4, 8 },
                         { 1, 0, 8, 2, 31, 8, 6, 3, 8 } } },
    { "Bell", 4, 0, { { 7, 1, 36, 2, 31, 10, 0, 15, 6 },
                      { 3, 0, 8, 2, 31, 9, 0, 15, 6 },
                      { 11, 5, 44, 2, 31, 10, 0, 15, 6 },
                      { 1, 0, 6, 2, 31, 8, 0, 15, 6 } } },
    { "Brass", 3, 5, { { 1, 0, 32, 1, 21, 6, 2, 3, 9 },
                       { 1, 3, 44, 1, 21, 6, 2, 3, 9 },
                       { 1, 5, 40, 1, 19, 5, 2, 2, 9 },
                       { 1, 0, 0, 1, 23, 3, 1, 1, 10 } } },
    { "Lead", 6, 5, { { 1, 0, 32, 1, 31, 4, 0, 1, 10 },
                      { 1, 0, 2, 1, 31, 0, 0, 0, 10 },
                      { 2, 0, 28, 1, 31, 0, 0, 0, 10 },
                      { 1, 1, 20, 1, 31, 0, 0, 0, 10 } } },
    { "Organ", 7, 0, { { 1, 0, 6, 0, 31, 0, 0, 0, 12 },
                       { 2, 0, 12, 0, 31, 0, 0, 0, 12 },
                       { 4, 0, 18, 0, 31, 0, 0, 0, 12 },
                       { 3, 0, 22, 0, 31, 0, 0, 0, 12 } } },
    { "Pad", 5, 2, { { 2, 0, 46, 0, 17, 3, 0, 2, 6 },
                     { 1, 2, 12, 0, 14, 2, 0, 1, 6 },
                     { 1, 6, 12, 0, 14, 2, 0, 1, 6 },
                     { 2, 0, 18, 0, 15, 2, 0, 1, 6 } } },
    { "Guitar", 2, 3, { { 3, 0, 32, 2, 31, 9, 7, 3, 9 },
                        { 1, 0, 40, 2, 31, 9, 6, 5, 9 },
                        { 2, 3, 44, 2, 31, 10, 7, 6, 9 },
                        { 1, 0, 0, 2, 31, 12, 8, 3, 9 } } },
    { "Pluck", 4, 2, { { 3, 0, 30, 2, 31, 17, 0, 15, 10 },
                       { 1, 0, 6, 2, 31, 15, 0, 15, 10 },
                       { 2, 5, 40, 2, 31, 16, 0, 15, 10 },
                       { 1, 0, 6, 2, 31, 14, 0, 15, 10 } } },
    { "Perc", 0, 7, { { 15, 0, 26, 3, 31, 16, 0, 15, 13 },
                      { 7, 3, 28, 3, 31, 16, 0, 15, 13 },
                      { 3, 5, 26, 3, 31, 15, 0, 15, 13 },
                      { 1, 0, 0, 3, 31, 10, 0, 15, 13 } } },
};
} // namespace retrochip
