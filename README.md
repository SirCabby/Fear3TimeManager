# Fear3TimeManager

A mission timer HUD for **F.E.A.R. 3** (Steam, PC): the time you have played the current mission
against the mission's target time, in the top-right corner, only while a mission is running.

```
12:34 / 30:00
```

- **Played time** is the game's own mission timer (the one the mission summary scores you on).
- **Target time** is the mission's par time for the difficulty you are playing. F.E.A.R. 3 stores
  four par times per mission (one per difficulty); the mod detects the mission and the difficulty and
  shows the right one. The played time turns red once it passes the target.
- **F10** pauses and resumes the mission timer. A pause is sticky: the mod holds the value until
  *you* resume it, through cutscenes, checkpoint reloads, the mission summary, the next level and a
  new game. The game restarts its own timer at every one of those; the hold puts it straight back.
  The HUD shows *(paused)*.
- Because a pause survives a level change, the held time can belong to an earlier mission. The
  control strip says so when it does; **Set played** re-bases it on the mission you are in.
- **F9** hides or shows the HUD. Its position, size and text scale come from the ini.
- **Pause the game (Esc)** and the HUD grows a control strip: pause/resume, hide/show, a box to set
  the played time (`mm:ss`, `h:mm:ss` or seconds), and the name of the detected mission with the
  ini key its target came from.

Works on Windows and on Linux/Proton with no launch options. Direct3D 11 (the default) and
Direct3D 9 (`-d3d9` in `options.cfg`) are both supported. It coexists with
[Fear3ChallengeGrant](https://github.com/SirCabby/Fear3ChallengeGrant): that mod proxies
`binkw32.dll`, this one proxies `fmodex.dll`.

## Install

1. Open your F.E.A.R. 3 folder (the one containing `F.E.A.R. 3.exe`).
2. Rename the existing `fmodex.dll` to `fmodex_orig.dll`.
3. Copy the mod's `fmodex.dll` in beside it.

Steam's *Verify integrity of game files* puts the stock DLL back; just repeat step 3 if that happens.
To uninstall, delete the mod's `fmodex.dll` and rename `fmodex_orig.dll` back.

## Config

`Fear3TimeManager.ini` is created next to the DLL on first run:

| Key | Default | Meaning |
| --- | --- | --- |
| `ToggleKey` | `0x78` (F9) | virtual-key code that shows/hides the HUD |
| `PauseKey` | `0x79` (F10) | virtual-key code that pauses/resumes the mission timer |
| `ShowOnStart` | `1` | HUD visible until hidden with the key |
| `PlayerIndex` | `0` | local player slot used for the in-mission check |
| `MarginX`, `MarginY` | `16` | distance from the top-right corner, in pixels |
| `Scale` | `1.0` | text scale of the HUD |
| `TargetPrison` … `TargetWard` | the game's par times | four seconds per mission, `Recruit,Commando,Fearless,Insane`; `0` = no par (Port) |
| `AlwaysShow` | `0` | debug: draw the HUD outside missions too |
| `Trace` | `0` | verbose diagnostics in `Fear3TimeManager.log` |
| `Disable` | | comma list of subsystems to turn off: `overlay,dispatch,game` |

`Fear3TimeManager.log` beside the DLL records what the mod found and did; attach it when reporting a
problem.

## How it works

The mod ships as a proxy `fmodex.dll` (the game's FMOD audio DLL, forwarded untouched through 77
generated jump thunks), so it loads before the game starts and needs no injector. It locates the
engine's score manager and menu manager at runtime — by their RTTI vtables, never by fixed
addresses; the Steam-protected executable is never modified — and reads the mission timer and its
running flag straight from the score manager. Pausing holds the timer's value in place, since the
game's own restart refuses to run in single player; the hold is re-asserted every tick, in a level
or not, so the resets the game does at a level start or a checkpoint load never end a pause. The current mission comes from the player
profile's level id, resolved through the level-list global data the game itself uses for mission
progression, and the current difficulty from the engine's difficulty component. The four par times
per mission were read out of the game's own world data, so the target is correct on every difficulty
without playing through. The HUD is Dear ImGui drawn from a hook on the swap chain's `Present`. See
`CLAUDE.md` for the reverse-engineering record.

## Building from source

Linux with mingw-w64 (`i686-w64-mingw32-g++`); no Windows or MSVC needed.

```sh
cp config.mk.example config.mk   # set GAME_DIR
make                             # build/fmodex.dll
make install                     # deploy into GAME_DIR (renames the stock DLL once)
make version 1.2.0               # set the version (VERSION file, baked into the DLL)
make package                     # dist/Fear3TimeManager_v<version>.zip
python3 tools/gen_proxy.py --exe "$GAME_DIR/F.E.A.R. 3.exe" --dll fmodex.dll \
    --def fmodex.def --inc src/proxy_exports.inc --prefix fmod   # regenerate the export list
```

Dear ImGui (MIT) is vendored under `contrib/imgui`. Licensed under the GPL-3.0; see `LICENSE`.
