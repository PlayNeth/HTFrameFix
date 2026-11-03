# HTFrameFix changelog

HTFrameFix is an ASI plugin for **Hydro Thunder** (`Hydro.exe`): an unlocked frame rate with interpolation in races, and widescreen.

Works with `Hydro.exe` SHA-1 `745cd3cf9aa0327fb5454a7763ff5f91bed9e3ed`. On any other exe, the mod leaves the game alone.

---

## 0.2: widescreen

- **Any resolution:** with the Direct3D renderer the game runs at your desktop's resolution, or `wsWidth` × `wsHeight`, instead of 640×480.
- **Hor+ 3D view:** the 4:3 vertical view stays, and you see more at the sides. The culling and the light glows are widened to match, and the sky is stretched across the width.
- **HUD:** 4:3 proportions, with its elements at the screen's left and right edges; messages stay in the middle (`wsHud`).
- **Markers for boats behind you** use the whole width: the ones off to the side sit at the screen's edges, and the row along the bottom spreads as wide.
- **Menus:** 4:3 in the middle with black bars, or stretched (`wsMenus`). Screen fades cover the whole screen, and menu images are scaled with the menus. Menus that draw a race scene behind their picture keep the bars black.
- **Settings:** `widescreen`, `wsWidth`, `wsHeight`, `wsFixFOV`, `wsHud`, `wsMenus`. They are added to an existing `HTFrameFix.ini` from 0.1.
- The DirectDraw renderers stay 640×480.
- **Sun flare and light glows** are now blended between ticks too. They placed themselves from the camera's own, unblended position, so they moved at 30 fps.
- **The sky** now scrolls smoothly too: it was set from the camera's angles once a tick.
- **Crash log:** if the game crashes, `HTFrameFix.log` records where and what the mod was doing.

## 0.1

The first release.

- **Races at the refresh rate:** in-between frames are drawn during races, at the display's refresh rate or `fpsLimit`. Each frame shows every object and the camera blended between the last two game ticks. The game's logic, physics and timing still run at 30 ticks a second.
- **Smooth objects:** the boats, scenery, arrow signs, balloons, paraglider and booster pickups.
- **Menus, the pause screen and the options** stay at the original 30 fps, and the sky is right after pausing.
- **Settings** in `HTFrameFix.ini`: `interpolate`, `fpsLimit`, `blendObjects`, `maxMove`, `maxTurn`, `logToFile`.
- Known limits: the small rotor stays at 30 fps (it turns exactly 180° a tick); spray and splash particles that appear or jump are drawn unblended for that tick; the picture is one tick (33 ms) behind the game.
