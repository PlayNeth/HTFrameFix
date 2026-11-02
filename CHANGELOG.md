# HTFrameFix changelog

HTFrameFix is an ASI plugin for **Hydro Thunder** (`Hydro.exe`): an unlocked frame rate with interpolation in races.

Works with `Hydro.exe` SHA-1 `745cd3cf9aa0327fb5454a7763ff5f91bed9e3ed`. On any other exe, the mod leaves the game alone.

---

## 0.1

The first release.

- **Races at the refresh rate:** in-between frames are drawn during races, at the display's refresh rate or `fpsLimit`. Each frame shows every object and the camera blended between the last two game ticks. The game's logic, physics and timing still run at 30 ticks a second.
- **Smooth objects:** the boats, scenery, arrow signs, balloons, paraglider and booster pickups.
- **Menus, the pause screen and the options** stay at the original 30 fps, and the sky is right after pausing.
- **Settings** in `HTFrameFix.ini`: `interpolate`, `fpsLimit`, `blendObjects`, `maxMove`, `maxTurn`, `logToFile`.
- Known limits: the small rotor stays at 30 fps (it turns exactly 180° a tick); spray and splash particles that appear or jump are drawn unblended for that tick; the picture is one tick (33 ms) behind the game.
