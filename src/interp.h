// What the other parts use from interp.cpp.
#pragma once
#include "common.h"
#include "game.h"

// A matrix as the game stores it: 26 floats, then the type (int).
struct Mat { float f[26]; int type; };
extern bool g_inDraw;
extern bool g_blend;
extern Mat g_viewExact;
extern bool g_viewSaved;
extern bool g_inDisplay;
// kDrawState, saved before the tick's draw (pre) and at the end of the tick
// (post); a frame shown starts from pre and leaves post. Each range is taken
// in whole 64-byte blocks (as tested).
constexpr DWORD BlockLo(const ht::Range& r) { return r.addr & ~63u; }
constexpr DWORD BlockHi(const ht::Range& r) { return (r.addr + r.len + 63u) & ~63u; }
constexpr DWORD StateBytes() {
    DWORD n = 0;
    for (const ht::Range& r : ht::kDrawState) n += BlockHi(r) - BlockLo(r);
    return n;
}
extern BYTE g_statePre[StateBytes()], g_statePost[StateBytes()];
extern bool g_preValid, g_postValid;
extern bool g_active;
void Install();
