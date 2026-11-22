// HTFrameFix.asi  v1.0.0
// Frame-rate unlock with interpolation, widescreen, arcade sound and arcade
// settings for Hydro Thunder (PC, Eurocom 2000)
//
// Analysed exe: Hydro.exe SHA-1 745cd3cf9aa0327fb5454a7763ff5f91bed9e3ed
// (PE timestamp 0x38EB1E5C, image base 0x400000, no ASLR). Every patch site is
// byte-checked first; on any other exe the ASI does nothing.
//
// How the game keeps time (main loop 0x47E68D: call 0x471AB0 / jmp back):
//  - One loop = one 30 Hz game tick. 0x471AB0 pumps messages and reads input
//    (0x47E270), runs the tick (0x471B80 -> 0x476930 -> 0x42F700), sends the
//    frame's polygons to the renderer (0x478110), flips (0x478040, Flip with
//    DDFLIP_WAIT), then Sleeps until 33333 us have passed.
//  - If a loop took more than 50 ms the next one is a "slow" frame: [0xF90C04]
//    = 1 skips drawing (0x47B320 drops polygons) and the flip.
//  - 0x42F700 runs the current mode's update function ([0x14549C0], ecx =
//    [0x14549C8]) and then its draw function ([0x14549BC], ecx = [0x14549B8],
//    called at 0x42F78A); afterwards 0x42F940 advances the tick counter
//    [0x145499C]. The race uses update 0x44C7D0 / draw 0x44C9C0.
//  - The camera sets the view through 0x469470 (base of the matrix stack,
//    0x144FA60), and every model is placed by multiplying matrices onto the
//    stack: 0x4694C0 / 0x469500 push one (ecx = 0x6C-byte matrix: rotation +0,
//    translation +0x24, inverse +0x30/+0x54, type +0x68), 0x4695B0 multiplies
//    two (also called directly by the models' code).
//
// What this ASI does (interpolate=1):
//  - The main loop runs ticks on an exact 30 Hz QPC schedule and draws a frame
//    every display refresh (or fpsLimit). In a race the tick's own draw is
//    hidden (the game's "skip drawing" flag, as in its slow frames) and only
//    records the exact matrices; every frame shown, on ticks too, is the race
//    draw called again, with the tick counter and its companions put back to
//    the values the tick's own draw saw, and every matrix blended between the
//    previous tick and this one by the time since the last tick (the frame
//    shows the scene one tick behind).
//  - A matrix is matched to the previous tick's by its call site and address
//    (closest one, the same object's first), by the object being drawn (the
//    caller's edi; for animated models the animation and the part), or by its
//    call site and order. It is blended only if it moved less than maxMove and
//    turned less than maxTurn; otherwise it is drawn as it is. Blends keep the
//    rotation rigid and rebuild the inverse from it. Matrices the game builds
//    to face the camera are not blended (they follow the blended view).
//  - The draw marks what it has drawn with the tick counter and skips anything
//    already marked (scene nodes, objects, mesh and light caches). Those reads
//    use a stamp of the mod's instead: the counter in ticks, a fresh value in
//    every in-between frame, so each frame draws the whole scene.
//  - Some of the race draw changes memory as it draws (the sky scrolls and
//    rescales its own vertices, the fog eases, a counter times a sound). That
//    memory is saved before and after the tick's draw; every in-between frame
//    starts from the first copy and leaves the second, so the game sees one
//    draw per tick.
//  - In-between frames are drawn only in races; menus, the pause screen and
//    the options keep 30 fps.
//
// widescreen=1: the Direct3D renderer runs at any resolution; see the
// Widescreen section.
// arcadeSound=1 with HTArcade.pak: the arcade's sounds, music and engines; see
// the Arcade sound section. trackDifficulty / aiDifficulty / arcadeSpeedway /
// arcadeLakePowell: see Game settings left from the arcade.

#include "src/common.h"
#include "src/config.h"
#include "src/log.h"
#include "src/patch.h"
#include "src/interp.h"
#include "src/widescreen.h"
#include "src/sound.h"
#include "src/settings.h"
#include "src/sun.h"
#include "src/crashlog.h"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    InitDirs();
    LoadConfig();
    if (g_cfg.logToFile) {
        char path[MAX_PATH]; snprintf(path, sizeof(path), "%sHTFrameFix.log", g_asiDir);
        g_log = fopen(path, "w");
    }
    Logf("HTFrameFix v" HTFF_VERSION "\n");
    if (!CheckSigs()) return TRUE;
    // (Only on the analysed exe: the crash log reads the game's globals.)
    if (g_log) AddVectoredExceptionHandler(0, CrashLog);
    Install();
    InstallWidescreen();
    InstallArcadeSound();
    InstallGame();
    InstallSun();
    if (g_active || g_wsOn) {
        if (PatchJmp(ws::glows, reinterpret_cast<void*>(&WsGlowStub), 5))
            Logf("light glows: %s\n", g_active ? "blended with the camera" : "widescreen only");
    }
    return TRUE;
}
