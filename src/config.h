// What the other parts use from config.cpp.
#pragma once
#include "common.h"

struct Config {
    int   interpolate = 1;
    int   fpsLimit = 0;          // 0 = refresh rate
    int   blendObjects = 1;      // 0 = only the camera is blended
    float maxMove = 100.0f;      // world units per tick; more = a jump (or another object), not blended
    float maxTurn = 50.0f;       // degrees per tick; more = not blended
    int   logToFile = 1;
    // widescreen (Direct3D renderer)
    int   widescreen = 1;
    int   wsWidth = 0;           // 0 = the desktop's resolution
    int   wsHeight = 0;
    int   wsFixFOV = 1;          // 1 = Hor+ 3D view; 0 = stretched
    int   wsHud = 1;             // 1 = 4:3 at the screen edges, 2 = 4:3 in the middle, 0 = stretched
    int   wsMenus = 1;           // 1 = 4:3 in the middle, 2 = stretched
    // sound
    int   arcadeSound = 1;       // 1 = sounds and music from HTArcade.pak when it is there
    int   arcadeSoundVolume = 70;   // percent: the pack's sound effects and speech
    int   arcadeMusicVolume = 70;   // percent: the music, while the pack is in use
    // sun
    int   sunBehindScenery = 1;  // 1 = scenery in front of the sun hides it
    // game
    int   trackDifficulty[13] = {};   // 1-100 (arcade operator setting); 0 = the game's (40)
    int   aiDifficulty[13] = {};      // 1-100; 0 = the game's (50)
    int   arcadeSpeedway = 0;    // 1 = the arcade's Hydro Speedway (left in the PC data, unused)
    int   arcadeLakePowell = 0;  // 1 = the arcade's Lake Powell
};
extern Config g_cfg;
extern char g_asiDir[MAX_PATH];
void InitDirs();
void LoadConfig();
