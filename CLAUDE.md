# Fear3TimeManager — project guide

A client-side mod for **F.E.A.R. 3** (Steam appid 21100, 32-bit `F.E.A.R. 3.exe`, Despair engine,
Steam CEG-protected), cross-built on Linux with mingw-w64. It draws a mission-timer HUD
(`played / target`) in the top-right corner during missions and can pause, resume and set the
mission timer. `README.md` is the user-facing doc; this file records **what was reverse-engineered**.
It is a sibling of `../Fear3ChallengeGrant`, whose `CLAUDE.md` holds the shared discoveries in full
(registration patterns, class descriptors vs instances, the heap-scan instance finder, renderer
capture, gotchas); only what is specific to the timer is repeated here.

## Build & deploy

```sh
make            # -> build/fmodex.dll   (config.mk sets GAME_DIR; gitignored)
make install    # rename stock fmodex.dll -> fmodex_orig.dll (once), deploy ours atomically
make uninstall  # restore the stock DLL
make version X.Y.Z   # set the version;  make package -> dist/Fear3TimeManager_vX.Y.Z.zip
python3 tools/gen_proxy.py --exe "$GAME_DIR/F.E.A.R. 3.exe" --dll fmodex.dll --def fmodex.def \
    --inc src/proxy_exports.inc --prefix fmod        # regenerate the proxy export list
python3 tools/find_regs.py --exe "$GAME_DIR/F.E.A.R. 3.exe" --name GetMissionTime   # etc.
```

The log (`Fear3TimeManager.log`) and ini (`Fear3TimeManager.ini`) sit beside the DLL in the game
folder. `Trace = 1` adds scan diagnostics; `AlwaysShow = 1` draws the HUD outside missions (rendering
test); `Disable = overlay,dispatch,game` bisects a fault.

`tests/test_adopt.cpp` runs the renderer hooks under Wine with the Steam overlay's way of hooking
played by the test (build and run commands in its header; `F3TM_ADOPT=1` makes the proxy take the
Windows path under Wine). See "The HUD's hooks on Windows".

## ⛔ Rules

- **Never patch `F.E.A.R. 3.exe` on disk** (CEG). Proxy DLL plus in-memory hooks only.
- **Never use absolute addresses.** Everything is found by signature at runtime from the method-name
  strings and verified against the bytes of the functions it is read from; VAs here document build
  id 3576 only.
- **Game calls only on the main thread** (`dispatch.cpp`'s `PeekMessageA` hook, primary thread only).
- **Never call a game function on a guessed layout.** Confirm from reflection registrations or the
  function bytes first (the challenge mod lost a restart cycle to exactly that).
- **On Windows, no function of the mod's in a vtable another hooker reads** (The HUD's hooks on
  Windows).
- The user commits every repo himself — do not `git commit`/`push` unless asked.

## Why fmodex.dll
`Fear3ChallengeGrant` already ships as `binkw32.dll`; two proxies of one DLL cannot coexist. The exe
imports 77 C++-mangled `__stdcall` methods from `fmodex.dll` (FMOD Ex), which ships with the game and
is not a Wine builtin. `tools/gen_proxy.py` reads the exe's import table and emits `fmodex.def`
(name -> `fmod_<n>` aliases; `.def` files accept mangled names verbatim) and
`src/proxy_exports.inc` (an X-macro list), and `src/proxy.cpp` turns every entry into a
`jmp [g_proxy_orig + n*4]` thunk resolved from `fmodex_orig.dll` in `DllMain`. Both mods hook the
same import slots and vtables (`GetProcAddress`, `PeekMessageA`, `IDXGISwapChain::Present`, the
window procedure); each calls the previous value, so they chain in load order.

## The timer (all on `FearScoreMgr`, primary vtable `0x18EAA1C`)
The `GameScriptFEARScore` wrappers (Pattern A registrations, see the sibling) each end in
`8B 11 8B 82 <slot*4> [5E] FF E0` on `[ctx+0x188]`, which gives the slots; the manager's own
functions at those slots give the fields:

| Wrapper | impl | slot | manager function (this build) |
| --- | --- | --- | --- |
| `StartMissionTimer` | `0xC182D0` | 54 | `0x793AC0`: gated on `[mgr+0x5C]->vtbl[48]()` false, `[mgr+0x9B]` (locked) 0 and `[mgr+0x9A]` (running) 0; sets running = 1 and calls `0x9E5E30` |
| `StopMissionTimer` | `0xDEDA40` | 55 | `0xD5B880`: if not locked and running: running = 0, destroys the timer receipt at `+0xB4` |
| `ResetMissionTimer` | `0x6331B0` | 56 | `0x866350`: if not locked, `[mgr+0x88] = 0.0f` |
| `GetMissionTime` | `0x849CE0` | 57 | `0x5A1A50`: `fld [mgr+0x88]; jmp _ftol` — **float seconds**, truncated to int for scripts |
| `GetParTime` | `0x50A6E0` | 58 | `0x5B38D0`: `mov eax,[mgr+0xC0]` — **int seconds** |
| `SetParTime(int)` | `0xB87280` | 59 | `0x766D50`: `[mgr+0xC0] = arg` (wrapper clamps to >= 0) |
| `GetGameLoadedSinceMissionStarted` | `0x79A270` | 60 | `0x672250`: `[mgr+0x9F]` byte |

So: played time = `float` at `mgr+0x88`, running flag byte `+0x9A`, lock byte `+0x9B`, par time
`int` at `+0xC0`. The per-frame update (`0x9985A7`: `fld dt; fadd [esi+0x88]; fstp [esi+0x88];
fld dt2; fadd [esi+0x8C]; fstp [esi+0x8C]`) shows the timer **accumulates `+= dt`** and that a
second, cumulative timer at `+0x8C` advances next to it.

**Do not use `StopMissionTimer`/`StartMissionTimer` to pause.** Stop works, but Start's first gate
(`[mgr+0x5C]->vtbl[48]()`) returns true in single player, so the timer never restarts (observed:
"resumed at 83 s" and the value stayed 83 for good). The freeze is therefore a *hold*: every tick
the mod writes the frozen values back into `+0x88` and `+0x8C` while the game keeps adding dt
(drift under one tick, invisible at whole seconds). Setting the time writes both floats.

**The hold is sticky and unconditional.** Only the user ends it (the pause key, the pause-menu
button; a new played time re-bases it). It is *not* gated on being in a level: the game resets and
restarts its own timer at a level start, a checkpoint load and a new game, so the hold has to be in
place through the loads and menus in between - an earlier build released it when
`HasPlayerStartedLevel` went false, which is exactly what un-paused the timer on every reload.
`hold_time` writes only when the field has drifted (so it is a read per tick while nothing moves the
timer) and returns what it found, which is how `dispatch.cpp` logs the game's own resets. The
value carries with the pause, so a pause taken in one mission holds that mission's time in the next
one; the control strip says so (`frozen_mission_index` vs `mission_index`) rather than re-basing
silently. A freeze never latches a failed read (`mission_time_f() < 0`), since that value would
be stamped back forever.

**Par time is set only at the mission-summary screen.** The level script `EndCurrentLevel.lua`
calls `GameScript.Support:ImportFloatVariable("Easy Par Time")` (a single value per mission, no
per-difficulty variants) and passes it to `SetParTime`, which writes `+0xC0`. That import runs at
mission end, not at load, and `SetParTime` was observed once, with 900, at the Prison summary. The
world tool-variable value is resident during play but stored keyed by a name hash, not adjacent to
the "Easy Par Time" string, so it is not cheaply readable mid-mission.

## The current mission (build 3576 addresses for verification only)

The par time cannot be read mid-mission, so the target comes from the ini, indexed by the mission
the mod detects. Two objects give it, both found by RTTI vtable scan:

- **`PlayerProfilePC`** (primary vtable `0x14EF59C`, a second at `+4`) carries **two** level ids:
  - `+0x11C` (setter slot 156 `0x8A2310`, getter slot 157 `0xBBBB30`) is the **current mission**.
    `PlayerProfileMgr`/`PlayerProfilePCMgr` vtable slot 38 (`0xB4ED50`, `SetCurrentLevel(levelId,
    flag)`) fetches the primary profile (`mgr->vtbl[30]`) and calls that setter, then broadcasts a
    profile message. The mod finds the manager function by its bytes (`8B 01 8B 54 24 ?? 8B 80
    <slot*4> 52 FF D0`; the generic shape occurs 21 times in `.text`, exactly once inside the
    manager's vtable functions), takes the setter slot from it and requires the setter/getter pair
    right there (`8B 44 24 04 89 81 <off> C2 04 00` / `8B 81 <off> C3`, same offset).
  - `+0x118` (slots 154/155) is the **progression** — the furthest mission reached. The core of
    `GameScriptGame::SetCurrentLevelId` (`0x6C33B0` -> `0xA538D0`) loops over the profile list
    (`[ctx+0x90]`, count `vtbl[10]`, get `vtbl[16]`) and only ever advances it (`8B 11 8B 82
    <slot*4> FF D0 8B 13 50 8B 42 0C ...`). First build read this one and reported Ward while
    Prison was loaded, because the profile had finished the game. Logged only.
  The base `PlayerProfile` returns `0x811C9DC5` (the hash of "") for both, i.e. "no level".
- **`LevelListGlobalDataComponent`** (primary vtable `0x1844180`; the
  `ILevelListGlobalDataComponent` interface is the subobject at `+0x24`, vtable `0x18440AC`). Its
  interface slot 3 (`0x80D910`) maps a level id to `nProgressionIndexValue`, slot 33 (`0x973830`)
  an index back to an id; both walk the `levelMap` (`this+0x38` from the property registration
  `push 0x38; push <scope>; push "levelMap"`): an intrusive list whose sentinel pointer sits at
  `iface+0x1C`, nodes `{next, ..., +0x10 LevelDescriptor}`. `LevelDescriptor` fields (from the
  property registrations near `0x1844218`): `nTextId/nameTextId +0x4`, `descriptionTextId +0x8`,
  `nProgressionIndexValue +0xC` (`nProgressionIndex` = that + 1), `strMissionName +0x10`
  (`std::string`, accessor `lea eax,[ecx+0x10]`), `strPreload +0x2C`, the level id `+0x48`
  (node `+0x58`), `tipTextIds +0x60`, `transFiles +0x70`, `strFilename` vector `+0x80/+0x84`,
  `previewImageResourceKey +0xA8`, `loadingScreenImageResourceKey +0xB8`,
  `nIntervalTitleTextId +0xF8`, `nIntervalDescTextId +0xFC`, `nIntervalNameTextId +0x100`,
  `nTargetScore +0x104`, `collectibles +0x108`. Two more lists hang off `iface+0x48` and
  `iface+0x74` (the other two maps at `this+0x64`/`+0x90`).

Level ids are **FNV-1** (multiply then xor, basis `0x811C9DC5`, prime `0x01000193`; `0x653FD0`)
of `strMissionName`, verified live: the campaign table holds exactly eight records named
`Mission #1` .. `Mission #8` with `nProgressionIndexValue` 0..7 in that order (the list is not
stored sorted), so index 0..7 = Prison, Slums, Store, Suburbs, Tower, Bridge, Port, Ward. The
other two lists hold MP maps (ids are small numbers, no names). The diagnostics dump logs whether
`FNV-1(strMissionName)` equals each record's id. `Trace = 1` dumps the profiles, the whole level table and the score manager on every level
start; the table is dumped once regardless, when it first becomes readable.

Dead ends worth not repeating: the global-data component keyed by `DAT_018438C8` is the
**MP** component (`MPGlobalDataComponent`), not a "current level" store; the static pair vector
at `0x183BD48` maps player index <-> player slot, not to a level; and `[ctx+0x238]` stores the
MenuMgr *`+4` subobject*, which is why an equality check against the primary pointer never found
the game context.

`MenuMgr::IsPauseMenuShowing` (slot 69 on the `+4` subobject: `[sub+0x60] != 0 && [sub+0x64] == 1`)
and `HasPlayerStartedLevel` (no `this`; `0x616950`) gate the HUD and the controls, as in the sibling.

## The HUD's hooks on Windows (src/adopt.cpp, overlay_dx11.cpp, overlay_dx9.cpp)
The renderer capture is Fear3ChallengeGrant's (the exe's `GetProcAddress` import hands back
wrappers; the DXGI factory's `CreateSwapChain`, then the swap chain's `Present` 8 / `ResizeBuffers`
13; D3D9: `IDirect3D9::CreateDevice` 16, then `EndScene` 42 / `Reset` 16). The account of the Windows crash is in Fear3CabbyCodes' `CLAUDE.md` ("The panel's hooks on Windows"):
the Steam overlay (`gameoverlayrenderer.dll`) hooks each new swap chain by writing a jump into
whatever function each slot of its vtable points to at that moment and keeps one saved original per
hook; the game makes its swap chain twice at start, and with the mod's Present/ResizeBuffers in
DXGI's class vtable the second pass took the mod's functions for its originals - the two called
each other until the stack ran out (`0xC00000FD`) before the first frame. Hence, on Windows only
(`adopt::enabled()`: not Wine, or `F3TM_ADOPT` in the environment), each swap chain the factory makes for
**the game's window** (a window of this process made by the thread that loaded the mod) and each
D3D9 device gets a private copy of its vtable with the mod's hooks (`src/adopt.cpp`; 64 / 192
slots), and the hooks call on through the vtable the object had, as it is at the time; the classes'
vtables are left alone. The factory's `CreateSwapChain` and `IDirect3D9::CreateDevice` stay hooked
in place (the overlay skips a factory slot outside `dxgi.dll` and wraps IDirect3D9 in its own
object). Under Wine the class vtables are hooked as before. On a process exit DllMain does nothing:
1.1.0's teardown there never finished (the log's last line was always `unloading - removing hooks`,
under Proton too; under Wine the test process hung there until killed).
`tests/test_adopt.cpp` plays the overlay: 1.1.0 loops (`LOOP`, D3D11 and D3D9) and hangs on exit;
the fix passes, and the Wine path still takes the class hook. Not yet seen in game on Windows.

## Input notes
- F10 arrives as `WM_SYSKEYDOWN` (Windows' menu key), not `WM_KEYDOWN`; the WndProc handles both for
  the hotkeys. During play no message is forwarded to ImGui at all (the HUD is draw-only); only
  while the game's pause menu is up does the window take input, so the game never loses a key.

## Tooling notes
- Ghidra headless takes script arguments as separate argv entries: a `$VAR` holding several
  `addr:` items must be word-split (zsh does not by default), or the script sees one bad number.
- `objdump` on the seven slot functions was enough here; they are getters/setters.

## Per-difficulty par times (from world data)

The par time is per-mission **and** per-difficulty. Each campaign world stores its four par times in
its tool-variable block as `<f32 value> DB 01 <namelen> <name>` records; the first is named
`Easy Par Time` and the next three follow it (Normal/Hard/Insane), 4 round floats in a row. Read from
`resources/Resource.dsPack` (uncompressed) and confirmed against play (Prison Recruit 900, Fearless
1320, Insane 1500). Difficulty index 0..3 = Recruit, Commando, Fearless, Insane (the world order
Easy/Normal/Hard/Insane). Values in seconds:

| Mission (index) | Recruit | Commando | Fearless | Insane | world tag |
| --- | --- | --- | --- | --- | --- |
| Prison (0)  | 900  | 1200 | 1320 | 1500 | prison_to_sewer |
| Slums (1)   | 1020 | 1320 | 1440 | 1620 | 01_End_Slums_Set |
| Store (2)   | 1200 | 1500 | 1620 | 1800 | 01_End_Store_Set |
| Suburbs (3) | 1500 | 2100 | 2400 | 2700 | 01_End_Village_Set (→ Needle) |
| Tower (4)   | 1800 | 2400 | 2700 | 3000 | Needle |
| Bridge (5)  | 1200 | 1800 | 2100 | 2400 | 01_End_Bridge_Set (→ port) |
| Port (6)    | — | — | — | — | no par record exists |
| Ward (7)    | 720  | 780  | 840  | 900  | WadeMansion (wade_ward_final) |

These are the mod's defaults (`config.h` `mission_target[8][4]`), editable per line in the ini as
`Target<Mission> = Recruit,Commando,Fearless,Insane`.

**Difficulty at runtime:** the engine's `DifficultyComponent` (RTTI `.?AVDifficultyComponent@Despair@@`,
primary vtable `0x183d8b4`) carries the current difficulty as an int at component+0x2C; its interface
subobject (offset 0x24, vtable `0x183d834`) exposes it via slot 0 `mov eax,[ecx+8]; ret`. The mod
finds the object by RTTI scan and reads that int (0..3). The script path is `GetDifficulty`
(`GameScript` global-data key `0x183dd28`, returns the int + 1).

**The par-computing script `EndCurrentLevel.lua` does not decompile offline:** its compiled chunk uses
a Despair-custom Lua 5.1 encoding (constant table desyncs at the same byte in both a hand parser and
unluac built from source). Not needed — the values were read straight from world data instead.
