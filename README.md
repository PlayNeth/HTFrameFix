# HTFrameFix

An unlocked frame rate with smooth interpolation, and widescreen, for **Hydro Thunder** on PC.

The game runs at a fixed 30 ticks a second and only draws a frame after each tick, so races are capped at 30 fps. HTFrameFix draws races at your display's refresh rate, blending the boats, the scenery and the camera between ticks, so they run smoothly at 60 Hz and higher. The game's logic, physics and timing still run at 30 ticks a second and aren't changed. It also runs the game at your screen's resolution instead of 640×480, with a Hor+ view and a HUD that isn't stretched. It's a single file, `HTFrameFix.asi`, and it doesn't touch the game's files on disk.

---

## Contents

- [Requirements](#requirements)
- [Installing](#installing)
- [Uninstalling](#uninstalling)
- [What it changes](#what-it-changes)
- [Settings](#settings)
- [Files the mod creates](#files-the-mod-creates)
- [Troubleshooting](#troubleshooting)
- [Building from source](#building-from-source)
- [How it works](#how-it-works)
- [License](#license)

---

## Requirements

- **Hydro Thunder** for PC, with this `Hydro.exe`: SHA-1 `745cd3cf9aa0327fb5454a7763ff5f91bed9e3ed` (PE timestamp `0x38EB1E5C`). Every place the mod patches is checked byte by byte first; with any other exe it writes a line to its log and does nothing.
- An **ASI loader**, such as [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader), installed as `winmm.dll` (the game imports WINMM, so that name loads).

It runs on Windows, and on Linux through Wine or Proton.

## Installing

1. Put the ASI loader (`winmm.dll`) in the game's folder, next to `Hydro.exe`.
2. Copy **`HTFrameFix.asi`** into the same folder.
3. Start the game.

On the first start the mod creates `HTFrameFix.ini`, with every setting explained, next to the `.asi`. To check it's working, look in `HTFrameFix.log`: it names the mod's version, says the exe matched, and gives the frame rate it draws at and the resolution.

Widescreen needs the game's **Direct3D** renderer (pick it in the game's setup). With the DirectDraw renderers the game stays 640×480.

## Uninstalling

Delete `HTFrameFix.asi`, and the ASI loader if nothing else uses it. You can also delete `HTFrameFix.ini` and `HTFrameFix.log`.

---

## What it changes

- **Races at your refresh rate:** in-between frames are drawn during races, at the display's refresh rate or a cap you set.
- **Everything moves smoothly:** the boats, the camera, the scenery, arrow signs, balloons, the paraglider, booster pickups, the sky, the sun flare and the light glows are all blended between ticks.
- **The game itself is unchanged:** its logic, physics and timing still run at 30 ticks a second, so races play exactly as before.
- **Menus stay as they were:** the menus, the pause screen and the options screen keep the original 30 fps.

### Widescreen

- **Any resolution:** the Direct3D renderer runs at your desktop's resolution, or the one you set with `wsWidth` and `wsHeight`, instead of 640×480.
- **The 3D view is Hor+:** the 4:3 vertical view stays, and you see more at the sides. The culling is widened to match, so nothing pops in at the edges, and the sky is stretched across the width.
- **The HUD** keeps its 4:3 proportions, with its elements at the screen's left and right edges. Messages stay in the middle.
- **Menus** are 4:3 in the middle with black bars, or stretched (`wsMenus=2`). Screen fades cover the whole screen.

There are no hotkeys.

### Known limits

- **The picture is one tick behind:** each frame shows the scene blended between the last two ticks, so it's 33 ms behind the game. That's inherent to this kind of interpolation.
- **The small rotor** turns exactly 180° every tick, so there's no in-between position to draw: it stays at 30 fps, as in the original.
- **Spray and splash particles:** one that appears, or jumps further than `maxMove` in a tick, is drawn without blending for that tick.
- **Widescreen and the HUD:** each HUD element goes to the left edge, the middle or the right edge depending on which third of the 4:3 screen it's in. Text is placed a whole string at a time.

---

## Settings

**`HTFrameFix.ini`** is next to the `.asi`. Every setting has a comment saying what it does.

| Setting | Default | What it does |
|---|---|---|
| `interpolate` | `1` | `1` draws in-between frames in races; `0` keeps the original 30 fps. |
| `fpsLimit` | `0` | `0` draws at the monitor's refresh rate; a number from 30 to 1000 caps the frame rate. |
| `blendObjects` | `1` | `1` smooths objects too; `0` smooths only the camera. |
| `maxMove` | `100` | An object that moves further than this in one tick (world units) is treated as having jumped, and is drawn where it is instead of blended. |
| `maxTurn` | `50` | An object that turns more than this in one tick (degrees, under 90) is drawn as it is. Animated parts are allowed up to 85°. |
| `logToFile` | `1` | Writes `HTFrameFix.log`. |
| `widescreen` | `1` | Master switch for the widescreen features; `0` keeps the original 640×480. |
| `wsWidth`, `wsHeight` | `0` | The resolution; `0` uses the desktop's. |
| `wsFixFOV` | `1` | `1` Hor+ 3D view; `0` stretched. |
| `wsHud` | `1` | `1` 4:3 HUD at the screen edges, `2` 4:3 HUD in the middle, `0` stretched. |
| `wsMenus` | `1` | `1` menus 4:3 in the middle, `2` stretched. |

[CHANGELOG.md](CHANGELOG.md) has the version history.

## Files the mod creates

All next to the `.asi`:

| File | |
|---|---|
| `HTFrameFix.ini` | Settings. |
| `HTFrameFix.log` | What the mod did, and where the game crashed if it did; send it with any bug report. |

## Troubleshooting

- **Nothing changed, and there's no `HTFrameFix.log`:** the ASI loader isn't loading. Check that it's in the game's folder as `winmm.dll`, and that `HTFrameFix.asi` is next to it.
- **The log says the exe differs and the mod is off:** your `Hydro.exe` isn't the one the mod was made for. Check it against the SHA-1 above. The mod leaves the game alone to be safe.
- **The game stays 640×480:** widescreen works with the Direct3D renderer only. The log says which resolution was set.
- **The game won't start in widescreen, or the screen stays black:** your system or wrapper may not offer that resolution in 16-bit colour. Set `wsWidth` and `wsHeight` to another resolution, or `widescreen=0`.
- **Tearing, or the frame rate runs too high:** turn on v-sync, or set `fpsLimit` to your refresh rate.
- **An object stutters or jumps:** it may move or turn more than `maxMove` or `maxTurn` in a tick; try raising them. `blendObjects=0` smooths only the camera.
- **Something feels off:** `interpolate=0` puts the original 30 fps back. Please report bugs with your `HTFrameFix.log`.

## Building from source

The source is a single file, `HTFrameFix.cpp`, built with MinGW-w64, 32-bit (GCC 13):

```
i686-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -shared -s -static-libgcc -static-libstdc++ \
  -o HTFrameFix.asi HTFrameFix.cpp -lwinmm
```

The code relies on GCC: AT&T inline asm stubs, `__attribute__((fastcall))` and `noinline`, and the `_` prefix of 32-bit Windows C symbols. MSVC would need the asm stubs rewritten.

## How it works

The comment at the top of `HTFrameFix.cpp` has the full version. In short:

- **Timing:** the game's main loop call is replaced. Ticks run on an exact 30 Hz schedule from the performance counter, and frames are drawn at the refresh rate.
- **The tick's own draw:** in a race, each tick's draw runs hidden, with the game's own "skip drawing" flag, and only records the exact matrices. Every frame shown is the race draw called again, with the tick counter put back to the value that tick's draw saw.
- **Blending:** every matrix the draw uses is caught where it's pushed onto the matrix stack or multiplied, and the camera view where it's set. Each is matched to the previous tick's by its call site and address, by the object being drawn (for animated models, the animation and the part), or by its call site and order, then blended by the time since the last tick. Rotations are kept rigid, and the stored inverse is rebuilt from the blended matrix. Matrices the game builds to face the camera aren't blended, since they already follow the blended view.
- **Widescreen:** the game lays everything out for 640×480, and its Direct3D renderer writes every triangle as screen-space vertices into one buffer. The mod sets the renderer's resolution, tags each triangle with what's drawing it (the race's 3D scene, the HUD, a menu) and moves it to the real screen when that buffer is drawn. The 3D scene is scaled around the middle, the HUD to the screen edges and menus to a 4:3 area in the middle. After the game sets up its projection, the mod widens the culling to the wider screen. The sky is a backdrop the game cuts into a fixed number of tiles, so it is stretched across instead. Images the menus copy straight into the frame go to a 640×480 copy that's scaled in afterwards.
- **Making the extra draws safe:** the draw marks scene nodes, objects and caches with the tick counter so nothing is drawn twice; those reads use the mod's own stamp, so each extra frame draws the whole scene again. The few areas of memory the race draw changes as it draws (the sky, the fog distances and a sound counter) are saved before and after the tick's draw and restored around every extra frame, so the game still sees exactly one draw per tick. Extra frames are only drawn after a tick the mod drew itself; the pause and options screens draw as the original does.

## License

GPL-3.0. See [LICENSE](LICENSE). Hydro Thunder is © Midway; this mod contains no game files.
