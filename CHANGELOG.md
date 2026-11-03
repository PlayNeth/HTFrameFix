# HTFrameFix changelog

HTFrameFix is an ASI plugin for **Hydro Thunder** (`Hydro.exe`): an unlocked frame rate with interpolation in races, widescreen, the arcade version's sound, and the arcade's difficulty settings and tracks.

Works with `Hydro.exe` SHA-1 `745cd3cf9aa0327fb5454a7763ff5f91bed9e3ed`. On any other exe, the mod leaves the game alone.

---

## 1.0.0

### Widescreen

- **Any resolution:** with the Direct3D renderer the game runs at your desktop's resolution, or `wsWidth` × `wsHeight`, instead of 640×480.
- **Hor+ 3D view:** the 4:3 vertical view stays, and you see more at the sides. The culling and the light glows are widened to match, and the sky is stretched across the width.
- **HUD:** 4:3 proportions, with its elements at the screen's left and right edges; messages stay in the middle (`wsHud`).
- **Markers for boats behind you** use the whole width: the ones off to the side sit at the screen's edges, and the row along the bottom spreads as wide.
- **Menus:** 4:3 in the middle with black bars, or stretched (`wsMenus`). Screen fades cover the whole screen, and menu images are scaled with the menus. Menus that draw a race scene behind their picture keep the bars black.
- The DirectDraw renderers stay 640×480.

### Interpolation

- **Sun flare and light glows** are now blended between ticks too. They placed themselves from the camera's own, unblended position, so they moved at 30 fps.
- **The sky** now scrolls smoothly too: it was set from the camera's angles once a tick.
- **Crash log:** if the game crashes, `HTFrameFix.log` records where and what the mod was doing.

### Sound and the arcade

- **Arcade sound (optional):** with `HTArcade.pak` next to the ASI, the arcade version's 16-bit 31250 Hz sounds and music replace the PC's 11 kHz 4-bit ones: 351 of the 425 sound files and 21 of the 34 music tracks. Arcade music loops seamlessly, and everything is mixed at 44.1 kHz instead of the 11025 Hz the game asks for (which Wine and DirectSound replacements honour). The game's pitch changes are kept, and each sound is set to the PC one's level.
- **Arcade engines:** the engine loops and ambient drones are the arcade's 16-bit ones too, and the four secret boats have their own engines again.
- **Arcade difficulty settings:** `trackDifficulty` (checkpoint time) and `aiDifficulty` (the other boats), 1-100, for every track or each one. The PC kept them without a menu.
- **Arcade Hydro Speedway and Lake Powell:** `arcadeSpeedway`, `arcadeLakePowell` race the arcade's versions of these tracks, which the PC data still has unused.
- **`tools/HTArcadePak.py`** builds `HTArcade.pak` from your own arcade hard disk image (`chdman extracthd` of the MAME CHD) or the `*.CSH` files from its D: drive and `HT.R2` from its C: drive. The mod contains no audio.
- **New settings:** `widescreen`, `wsWidth`, `wsHeight`, `wsFixFOV`, `wsHud`, `wsMenus`, `arcadeSound`, `trackDifficulty`, `aiDifficulty`, `arcadeSpeedway`, `arcadeLakePowell`. They are added to an existing `HTFrameFix.ini` from 0.1.

## 0.1

The first release.

- **Races at the refresh rate:** in-between frames are drawn during races, at the display's refresh rate or `fpsLimit`. Each frame shows every object and the camera blended between the last two game ticks. The game's logic, physics and timing still run at 30 ticks a second.
- **Smooth objects:** the boats, scenery, arrow signs, balloons, paraglider and booster pickups.
- **Menus, the pause screen and the options** stay at the original 30 fps, and the sky is right after pausing.
- **Settings** in `HTFrameFix.ini`: `interpolate`, `fpsLimit`, `blendObjects`, `maxMove`, `maxTurn`, `logToFile`.
- Known limits: the small rotor stays at 30 fps (it turns exactly 180° a tick); spray and splash particles that appear or jump are drawn unblended for that tick; the picture is one tick (33 ms) behind the game.
