# Mercenaries 2: World in Flames - Mercs2Fix

A single `.asi` plugin for the 2008 PC release of Mercenaries 2 that fixes the things
about it that are bad on a modern PC: windowed/borderless play, mounted-weapon aiming,
crouch aim lock, the car camera snap, and shadow map resolution.

It patches the game's own code at addresses verified byte-by-byte against the shipped
executable, so it needs no game files to be modified and nothing is written to disk.

**Target:** `Mercenaries2.exe`, 53,482,288 bytes, MD5 `857b3387d54774a32c1328effb5de4d4`
(the 32-bit Steam/Origin build). Other revisions may move the patched addresses; if the
log says a patch was skipped, it was skipped rather than guessed at.


## What it changes

- **Borderless windowed.** Forces the D3D9 device windowed and sizes the window to your
  monitor, at whatever resolution you pick in the game's own video options. Alt-tab is
  instant.
- **Mounted weapons aim at the speed you ask for.** The game caps a turret at 1.5 rad/s,
  adds inertia that makes the crosshair stay put during a fast sweep, and slows aim three
  separate ways whenever it thinks the mount is manned. All four are fixed. This is the
  big one for artillery and the boat gunner.
- **Look up and down while crouching or sliding.** The game throws the pitch delta away on
  that camera branch. It no longer does.
- **Car camera.** Its re-centre is slowed into a hold plus a blend instead of a snap.
- **Aim assist.** The correction the game adds to your mouse yaw is removed, and a held
  lock-on key stops eating the frame's aim input.
- **Shadows.** Atlas resolution (x4 in the shipped config) and cast distance.
- **Settings panel.** Press **Insert** in game. Every row takes effect while you are
  standing in the world, and by default is written back to `Mercs2Fix.ini`.
- **F8** reverts the two turret rows for the rest of the run, so "is this better than
  stock" is one session rather than two.

It does **not** touch your mouse input. An earlier generation of this mod injected raw
mouse deltas into the game's input stream; that was measured, found to be interference
rather than a fix, and has been deleted. The aim-feel improvement comes from the camera
patches above. The game aims by polling the Windows cursor position and re-pinning it
itself, which is why anything that fought that path only ever made aiming worse.


## Requirements

Both are third-party and neither is included here.

1. **Ultimate ASI Loader** - loads `Mercs2Fix.asi` into the game.
   <https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases>
   Take the **32-bit (x86)** build and install it as `dinput8.dll` in the game folder
   (the loader's own rename-as-a-proxy mechanism is what this plugin relies on).
2. **DXVK, 32-bit (x86)** - **required for water**, recommended otherwise.
   <https://github.com/doitsujin/DXVK>
   Put its `d3d9.dll` in the game folder. Mercenaries 2's stock D3D9 path renders Lake
   Maracaibo incorrectly on modern NVIDIA drivers - the lake bed is dry and the walls are
   vertical. That is a driver-era bug in the game's own shader path, not a settings
   problem; DXVK 3.1.1 x86 renders it correctly.


## Install

```
<game folder>\
  Mercenaries2.exe
  dinput8.dll          <- Ultimate ASI Loader (x86)
  d3d9.dll             <- DXVK (x86), for correct water
  Mercs2Fix.asi        <- from the release archive, or build it
  Mercs2Fix.ini        <- the config, edit or use the Insert panel
  data\shader3.bin     <- optional, x4 shadows: see shader\INSTALL.txt
```

1. Copy `Mercs2Fix.asi` into the game folder. It is in the release archive; build it
   yourself with `build.bat` if you prefer.
2. Put `Mercs2Fix.ini` beside it.
3. Install the ASI loader as `dinput8.dll`.
4. For x4 shadows, follow `shader\INSTALL.txt` (game closed, two file copies). Skip it and
   you keep stock shadow resolution; nothing else in the mod needs it.
5. Start the game. Open `Mercs2Fix.log` in the same folder if anything looks off - the
   first lines say which version loaded and which patches were applied or skipped.


## Shadow resolution

The shadow map has two halves and **they have to agree**:

| half | what it is | how you change it |
| --- | --- | --- |
| the atlas the game allocates | literal constants in `Mercenaries2.exe` | `Mercs2Fix.asi`, from `[Shadow] MapSizeScale` |
| the PCF filter that reads it | texel size baked into `data\shader3.bin` | the file below, or `tools\shadow_res.py` |

Move one without the other and the filter samples the wrong grid - blocky or
stair-stepped shadows, worse than stock. `Mercs2Fix.log` names which half disagrees.

**To install the x4 shaders, follow `shader\INSTALL.txt`** - it is the whole procedure, with
the two log lines that confirm success and the undo. Short version, game closed:

1. Copy `data\shader3.bin` to `data\shader3.bin.orig`.
2. Copy `shader\shader3.x4.bin` to `data\shader3.bin` - **keep the name `shader3.bin`**.
3. Leave `[Shadow] MapSizeScale=4` in `Mercs2Fix.ini` (it is the shipped value).
4. Start the game and look for `ShadowScale: x4 on 15/15 immediates` in `Mercs2Fix.log`.

Prefer to edit your own file instead of replacing it? `python tools\shadow_res.py set 4`
does exactly that, in place, and keeps its own backup (`probe`, `verify` and `restore` are
its other commands). It only rewrites the baked float constants, so every blob offset, size
and hash key stays valid. If your game is not installed where the tool expects, point it at
the file with the `MERCS2_SHADER` environment variable.

The shipped `Mercs2Fix.ini` assumes x4 on both halves, so the ini and the shader file are a
matched pair - change one, change the other.


## Known issue: raising draw distance

`[View] Distance` works - the game's far clip plane is `400 + 20 x Distance` - but above
roughly 300 **some dynamic objects (the player, mounted weapons) disappear at certain
camera angles and hard-pop back when you move**, while their interaction prompt stays on
screen. It happens on any map, mission or not, and the objects that go are consistent for
a given spot.

This is a bug in the game. Every CPU-side reason an object could stop being drawn was
instrumented on the live process and measured out: the LOD draw gate, the view frustum and
clip volume, the draw-batch arena's capacity, object presence in the scene set, the
settings block the draw-distance key writes into, the skinned-object submit path, and the
D3D9 user clip planes. The shipped build also disassembles the entire shader library - no
vertex shader self-transforms or compares a clip position, and no `oPos` write sits under
flow control. Each of those was a candidate and each is cleared with numbers, in the log.
What is left is above the reach of the patches this file can honestly make, so draw
distance ships at 100 (stock) and the lever is documented rather than capped.


## Build from source

```
build.bat
```

That is Visual Studio 2022's x86 developer environment (`vcvars32.bat`) plus one `cl`
command; `build.bat` calls it for you and writes `build\Mercs2Fix.asi`. 32-bit only - the
game is 32-bit. No dependencies beyond the Windows SDK (DirectInput/D3D9 headers).


## Files

```
src\Mercs2Fix.cpp              the plugin, one translation unit
build.bat                      builds it
Mercs2Fix.ini                  the config users actually edit
docs\Mercs2Fix.annotated.ini   every key, plus the RE notes behind each patch
docs\re-camera-recenter-asi.md the car-camera analysis
tools\shadow_res.py            the shader-half tool
shader\shader3.x4.bin          pre-baked x4 shadow shaders
shader\INSTALL.txt             how to install them, and how to undo it
```


## License

MIT, Copyright (c) 2026 HRVAT007. See `LICENSE.txt`.

Not affiliated with EA or Pandemic Studios. This mod requires you to own the game and
makes no change to its copy protection.
