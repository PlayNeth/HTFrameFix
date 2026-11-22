#include "common.h"
#include "settings.h"
#include "game.h"
#include "config.h"
#include "log.h"
#include "patch.h"

// ---------------------------------------------------------------------------
// Game settings left from the arcade
// ---------------------------------------------------------------------------
// The PC version keeps the arcade's operator settings (0x1455420, loaded from
// Hydro.cfg at start-up by 0x428EC0) without the operator menu: a track
// difficulty per track (+0x50, the checkpoint times: 0x451B60 picks them from a
// table, 0 = most time, 99 = least) and an AI difficulty per track (+0x84). The
// AI's are put back to 50 on every start, the track ones stay at 40. Both are
// set from the ini once the settings are loaded.
// The PC data also still has the arcade's own Hydro Speedway and Lake Powell
// (HETXTR2 / HFTXTR3, with their own skies and water); the PC's track table
// points at reworked ones. Their slots can be pointed back.
static void ApplyDifficulty() {
    for (int t = 0; t < 13; t++) {
        int d = g_cfg.trackDifficulty[t], a = g_cfg.aiDifficulty[t];
        if (d > 0) G<int>(0x1455470 + t * 4) = (d > 100 ? 100 : d) - 1;
        if (a > 0) G<int>(0x14554A4 + t * 4) = a > 100 ? 100 : a;
    }
}
// replaces the call at 0x428ED4 (to 0x402910, a bare ret) at the end of the settings' load
static const char kXtr2[] = "HETXTR2TRH0", kXtr3[] = "HFTXTR3TRH0";
static const ht::Sig kGameSigs[] = {
    { 0x428ED4, "\xE8\x37\x9A\xFD\xFF\xB8\x01\x00\x00\x00", 10 },   // operator settings loaded: call 0x402910
    { 0x402910, "\xC3", 1 },                                         //   (ret)
    { 0x43C8FF, "\x8B\x04\x85\x70\x54\x45\x01", 7 },                // track difficulty read
    { 0x4EF69C, "\xD8\xF8\x4E\x00", 4 },                            // Hydro Speedway's track file
    { 0x4EEE3C, "\x10\xFA\x4E\x00", 4 },                            // Lake Powell's
    { 0x4EF8D8, "HZTLOO2TRH0", 11 },
    { 0x4EFA10, "HPTPWELTRH0", 11 },
};
void InstallGame() {
    bool diff = false;
    for (int t = 0; t < 13; t++) diff |= g_cfg.trackDifficulty[t] > 0 || g_cfg.aiDifficulty[t] > 0;
    if (!diff && !g_cfg.arcadeSpeedway && !g_cfg.arcadeLakePowell) return;
    if (!SigsMatch(kGameSigs, sizeof(kGameSigs) / sizeof(kGameSigs[0]))) { Logf("arcade settings and tracks: off\n"); return; }
    if (diff && PatchCall(0x428ED4, reinterpret_cast<void*>(&ApplyDifficulty), 5)) {
        char a[80] = "", b[80] = "";
        for (int t = 0; t < 13; t++) {
            snprintf(a + strlen(a), sizeof(a) - strlen(a), "%s%d", t ? "," : "", g_cfg.trackDifficulty[t]);
            snprintf(b + strlen(b), sizeof(b) - strlen(b), "%s%d", t ? "," : "", g_cfg.aiDifficulty[t]);
        }
        Logf("difficulty: tracks %s, AI %s (0 = the game's)\n", a, b);
    }
    DWORD p2 = reinterpret_cast<DWORD>(kXtr2), p3 = reinterpret_cast<DWORD>(kXtr3);
    if (g_cfg.arcadeSpeedway && PatchBytes(0x4EF69C, &p2, 4)) Logf("Hydro Speedway: the arcade's\n");
    if (g_cfg.arcadeLakePowell && PatchBytes(0x4EEE3C, &p3, 4)) Logf("Lake Powell: the arcade's\n");
}
