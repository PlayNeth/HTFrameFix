#include "common.h"
#include "config.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

Config g_cfg;
char g_asiDir[MAX_PATH] = {};

void InitDirs() {
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&InitDirs), &self) &&
        GetModuleFileNameA(self, g_asiDir, MAX_PATH)) {
        char* s = strrchr(g_asiDir, '\\'); if (s) s[1] = 0; else g_asiDir[0] = 0;
    }
}

static const char kIniText[] =
    "; HTFrameFix.ini (v" HTFF_VERSION ") - Hydro Thunder (PC)\n"
    "; the game runs at 30 ticks a second. 1 = in-between frames are drawn in races, so they run\n"
    ";   at the monitor's refresh rate; 0 = original (30 fps)\n"
    "interpolate=1\n"
    "; frame cap with interpolate=1: 0 = monitor refresh rate, or a number (30-1000)\n"
    "fpsLimit=0\n"
    "; 1 = objects move smoothly too; 0 = only the camera (objects move at 30 fps)\n"
    "blendObjects=1\n"
    "; an object that moves more than this between two ticks (world units) has jumped, and is\n"
    ";   drawn where it is instead of blended\n"
    "maxMove=100\n"
    "; an object that turns more than this between two ticks (degrees, under 90) is drawn as it is\n"
    "maxTurn=50\n"
    "; write HTFrameFix.log next to the ASI\n"
    "logToFile=1\n";

static const char kWsBlock[] =
    "; --- widescreen (Direct3D renderer) ---\n"
    "; 1 = the game runs at the resolution below instead of 640x480, at any aspect ratio;\n"
    ";   0 = the original 640x480\n"
    "widescreen=1\n"
    "; resolution; 0 = the desktop's\n"
    "wsWidth=0\n"
    "wsHeight=0\n"
    "; 1 = Hor+ 3D view (the 4:3 vertical view stays, you see more at the sides); 0 = stretched\n"
    "wsFixFOV=1\n"
    "; 1 = 4:3 HUD, its elements at the screen edges; 2 = 4:3 HUD in the middle; 0 = stretched\n"
    "wsHud=1\n"
    "; 1 = menus 4:3 in the middle; 2 = stretched\n"
    "wsMenus=1\n";

static const char kArcadeBlock[] =
    "; --- arcade sound ---\n"
    "; 1 = the arcade version's sounds and music (31250 Hz, from HTArcade.pak next to the ASI,\n"
    ";   built by HTArcadePak from the arcade hard disk) replace the PC's 11 kHz ones; 0 = PC sounds\n"
    "arcadeSound=1\n";

static const char kArcadeVolBlock[] =
    "; how loud the arcade sound effects and speech are, in percent of the pack's own level\n"
    ";   (1-100). Full level overloads the mix when many play at once.\n"
    "arcadeSoundVolume=70\n"
    "; how loud the music is while the pack is in use, in percent (1-100)\n"
    "arcadeMusicVolume=70\n";

static const char kSunBlock[] =
    "; --- sun ---\n"
    "; 1 = cliffs and buildings in front of the sun hide it; 0 = the sun shows through them\n"
    ";   (the game's)\n"
    "sunBehindScenery=1\n";

static const char kGameBlock[] =
    "; --- arcade settings and tracks ---\n"
    "; the arcade's operator difficulty settings, which the PC version keeps but has no menu for.\n"
    "; trackDifficulty: how much time the checkpoints give, 1 = most time, 100 = least; the game\n"
    ";   uses 40. aiDifficulty: how hard the other boats race, 1-100; the game uses 50.\n"
    "; one number for every track, or 13 separated by commas, in this order: Ship Graveyard, Lost\n"
    ";   Island, Venice Canals, Lake Powell, Arctic Circle, Nile Adventure, New York Disaster,\n"
    ";   Greek Isles, Far East, Catacomb, Thunder Park, Hydro Speedway, Castle Von Dandy.\n"
    "; 0 = the game's\n"
    "trackDifficulty=0\n"
    "aiDifficulty=0\n"
    "; 1 = race the arcade version of Hydro Speedway / Lake Powell instead of the PC's reworked one\n"
    ";   (the PC data still has them, unused)\n"
    "arcadeSpeedway=0\n"
    "arcadeLakePowell=0\n";

static void ParseList(const char* v, int* out, int n) {           // "40" or "40,45,..."
    int i = 0, last = 0;
    while (*v && i < n) {
        last = atoi(v); out[i++] = last;
        const char* c = strchr(v, ','); if (!c) break;
        v = c + 1;
    }
    if (i == 1) for (; i < n; i++) out[i] = last;
}

void LoadConfig() {
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%sHTFrameFix.ini", g_asiDir);
    FILE* f = fopen(path, "r");
    if (!f) {
        if ((f = fopen(path, "w"))) {
            fputs(kIniText, f); fputs("\n", f); fputs(kWsBlock, f);
            fputs("\n", f); fputs(kArcadeBlock, f); fputs(kArcadeVolBlock, f);
            fputs("\n", f); fputs(kSunBlock, f);
            fputs("\n", f); fputs(kGameBlock, f); fclose(f);
        }
        return;
    }
    bool sawWs = false, sawArcade = false, sawGame = false, sawVol = false, sawSun = false;
    char line[MAX_PATH + 64];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = line; while (*key == ' ' || *key == '\t') key++;
        char* ke = eq; while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        char* val = eq + 1; while (*val == ' ' || *val == '\t') val++;
        if (char* c = strchr(val, ';')) *c = 0;
        char* ve = val + strlen(val);
        while (ve > val && (ve[-1] == '\r' || ve[-1] == '\n' || ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;
        int n = atoi(val);
        if      (!_stricmp(key, "interpolate"))  g_cfg.interpolate = n;
        else if (!_stricmp(key, "fpsLimit"))     g_cfg.fpsLimit = n;
        else if (!_stricmp(key, "blendObjects")) g_cfg.blendObjects = n;
        else if (!_stricmp(key, "maxMove"))      { float v = (float)atof(val); if (v > 0.0f) g_cfg.maxMove = v; }
        else if (!_stricmp(key, "maxTurn"))      { float v = (float)atof(val); if (v > 0.0f && v < 90.0f) g_cfg.maxTurn = v; }
        else if (!_stricmp(key, "logToFile"))    g_cfg.logToFile = n;
        else if (!_stricmp(key, "widescreen"))   { g_cfg.widescreen = n; sawWs = true; }
        else if (!_stricmp(key, "wsWidth"))      g_cfg.wsWidth = n;
        else if (!_stricmp(key, "wsHeight"))     g_cfg.wsHeight = n;
        else if (!_stricmp(key, "wsFixFOV"))     g_cfg.wsFixFOV = n;
        else if (!_stricmp(key, "wsHud"))        g_cfg.wsHud = n;
        else if (!_stricmp(key, "wsMenus"))      g_cfg.wsMenus = n;
        else if (!_stricmp(key, "arcadeSound"))  { g_cfg.arcadeSound = n; sawArcade = true; }
        else if (!_stricmp(key, "arcadeSoundVolume")) { if (n >= 1 && n <= 100) g_cfg.arcadeSoundVolume = n; sawVol = true; }
        else if (!_stricmp(key, "arcadeMusicVolume")) { if (n >= 1 && n <= 100) g_cfg.arcadeMusicVolume = n; }
        else if (!_stricmp(key, "sunBehindScenery")) { g_cfg.sunBehindScenery = n; sawSun = true; }
        else if (!_stricmp(key, "trackDifficulty")) { ParseList(val, g_cfg.trackDifficulty, 13); sawGame = true; }
        else if (!_stricmp(key, "aiDifficulty")) ParseList(val, g_cfg.aiDifficulty, 13);
        else if (!_stricmp(key, "arcadeSpeedway")) g_cfg.arcadeSpeedway = n;
        else if (!_stricmp(key, "arcadeLakePowell")) g_cfg.arcadeLakePowell = n;
    }
    fclose(f);
    if (!sawWs || !sawArcade || !sawGame || !sawVol || !sawSun) {   // an older ini: add the new settings
        if (FILE* a = fopen(path, "a")) {
            if (!sawWs) { fputs("\n", a); fputs(kWsBlock, a); }
            if (!sawArcade) { fputs("\n", a); fputs(kArcadeBlock, a); }
            if (!sawVol) { if (sawArcade) fputs("\n", a); fputs(kArcadeVolBlock, a); }
            if (!sawSun) { fputs("\n", a); fputs(kSunBlock, a); }
            if (!sawGame) { fputs("\n", a); fputs(kGameBlock, a); }
            fclose(a);
        }
    }
}
