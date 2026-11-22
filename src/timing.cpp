#include "common.h"
#include "timing.h"
#include "config.h"
#include "log.h"

// ---------------------------------------------------------------------------
// Game functions
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
// The game sets the FPU to 24-bit precision (0x476900), so times stay integers
// and only small differences become floats.
LONGLONG g_qpf = 0, g_tickPeriod = 0, g_framePeriod = 0;

void WaitUntil(LONGLONG t) {
    LONGLONG now = Qpc();
    while (t - now > 0) {
        if (t - now > g_qpf / 500) Sleep(1);                        // more than 2 ms left
        else YieldProcessor();
        now = Qpc();
    }
}

static int g_refreshHz = 0;
static DWORD g_refreshCheckMs = 0;
void UpdateFramePeriod() {
    DWORD ms = GetTickCount();
    if (g_framePeriod && ms - g_refreshCheckMs < 1000) return;      // the mode can change: look again every second
    g_refreshCheckMs = ms;
    int fps = g_cfg.fpsLimit;
    if (fps <= 0) {
        DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
        fps = EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmDisplayFrequency : 0;
        if (fps <= 1) fps = 60;
    }
    if (fps < 30) fps = 30;
    if (fps > 1000) fps = 1000;
    if (fps != g_refreshHz) {
        g_refreshHz = fps;
        Logf("frame rate: %d fps%s\n", fps, g_cfg.fpsLimit > 0 ? " (fpsLimit)" : " (refresh rate)");
    }
    g_framePeriod = g_qpf / fps;
}
