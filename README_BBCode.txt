[size=6][b]Fear3TimeManager[/b][/size]

A [b]mission timer HUD[/b] for F.E.A.R. 3: the time you have played the current mission against the mission's target time, in the top-right corner, only while a mission is running.

[b]12:34 / 30:00[/b] - played time / target time. The played time turns red once it passes the target.

This mod is open source! Check it out at [url=https://github.com/SirCabby/Fear3TimeManager]https://github.com/SirCabby/Fear3TimeManager[/url]

[size=5][b]What it does[/b][/size]
[list]
[*]Shows the game's own [b]mission timer[/b] (the one the mission summary scores you on) next to the mission's [b]target time[/b]. The mission and difficulty are detected automatically, and the four par times per mission (one per difficulty) were read from the game's world data, so the target is right on every difficulty. All values are editable in the ini.
[*][b]F10[/b] pauses and resumes the mission timer. A pause is sticky: the mod holds the value until you resume it, through cutscenes, checkpoint reloads, the next level and a new game - the game restarts its own timer at every one of those and the hold puts it straight back. Since the time can then belong to an earlier mission, the control strip says so, and [b]Set played[/b] re-bases it.
[*][b]F9[/b] hides or shows the HUD.
[*][b]Pause the game (Esc)[/b] for a small control strip: pause/resume, hide/show, and a box to type a new played time (mm:ss, h:mm:ss or seconds).
[/list]

[size=5][b]Install[/b][/size]

Copy into your F.E.A.R. 3 folder (the one with F.E.A.R. 3.exe):
[list=1]
[*]Rename the stock [b]fmodex.dll[/b] to [b]fmodex_orig.dll[/b]
[*]Drop this mod's [b]fmodex.dll[/b] in its place
[/list]

That is the whole install, on [b]Windows and Linux/Proton alike[/b] - no launch options, no WINEDLLOVERRIDES, no ASI loader. It works side by side with Fear3ChallengeGrant.

To uninstall: delete fmodex.dll and rename fmodex_orig.dll back.

[size=5][b]Please read before using[/b][/size]
[list]
[*][b]Pausing or setting the timer changes the mission time the game scores you on[/b] (the time bonus in the mission summary). Nothing is written to your saves by the mod itself.
[*][b]Steam's "Verify integrity of game files" removes the mod[/b] by restoring the stock DLL. Just re-copy the file.
[/list]

[size=5][b]Config[/b][/size]

[code]
ToggleKey   = 0x78   ; virtual-key code that shows/hides the HUD (0x78 = F9)
PauseKey    = 0x79   ; virtual-key code that pauses/resumes the mission timer (0x79 = F10)
ShowOnStart = 1      ; HUD visible until hidden with ToggleKey
MarginX     = 16     ; distance from the right edge of the screen, in pixels
MarginY     = 16     ; distance from the top edge of the screen, in pixels
Scale       = 1.0    ; text scale of the HUD
TargetPrison = 900,1200,1320,1500   ; per mission: Recruit,Commando,Fearless,Insane (seconds)
                                    ; TargetSlums/Store/Suburbs/Tower/Bridge/Port/Ward likewise
[/code]

Lives in [b]Fear3TimeManager.ini[/b] next to F.E.A.R. 3.exe (created on first run). [b]Fear3TimeManager.log[/b] beside it records what the mod found and did - attach it when reporting a problem.
