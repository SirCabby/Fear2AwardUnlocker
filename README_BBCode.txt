[size=6][b]Fear2AwardUnlocker[/b][/size]

An in-game mod that adds a [b]panel of toggles for every award[/b] in F.E.A.R. 2: Project Origin, letting you flip each one [b]on or off[/b] - individually, or all at once.

[b]It opens itself on the Awards screen[/b] - nothing to set up, nothing to press. Press [b]F8[/b] to open it anywhere else in the game.

This mod is open source! Check it out at [url=https://github.com/SirCabby/Fear2AwardUnlocker]https://github.com/SirCabby/Fear2AwardUnlocker[/url]

[size=5][b]What it does[/b][/size]
[list]
[*]A panel with a [b]toggle for every one of the 58 awards[/b] - it appears on its own when you reach the Awards screen, or on [b]F8[/b] anywhere else.
[*][b]Unlock[/b] or [b]lock[/b] any award, or use [b]Unlock all[/b] / [b]Lock all[/b] to do the lot at once.
[*][b]Filter[/b] box to find an award quickly.
[*]The Awards screen updates immediately, and changes are saved.
[/list]

[size=5][b]Steam-safe[/b][/size]

F.E.A.R. 2 has [b]no Steam achievements at all[/b] - there is nothing for this mod to touch on Steam even in principle. It drives only the game's own local award state, the same state the Awards screen reads.

[size=5][b]Install[/b][/size]

Copy into your F.E.A.R. 2 folder (the one with FEAR2.exe):
[list=1]
[*]Rename the stock [b]ltmemory.dll[/b] to [b]LTMemory_orig.dll[/b]
[*]Drop this mod's [b]LTMemory.dll[/b] in its place
[/list]

That is the whole install, on [b]Windows and Linux/Proton alike[/b] - no launch options, no WINEDLLOVERRIDES, no ASI loader.

To uninstall: delete LTMemory.dll and rename LTMemory_orig.dll back.

[size=5][b]Please read before using[/b][/size]
[list]
[*][b]Every award is written into the save, the nine Reborn (DLC) ones included.[/b] The game keeps one byte per award in Profile000.gps - the same byte it writes when you earn one - and that is what the panel toggles. Nothing is kept mod-side and nothing is re-applied at launch: changes survive a restart, and survive removing the mod.
[*][b]Toggling an award OFF does not undo the progress behind it.[/b] The mod clears the award, not the counter it was earned from, so an award whose condition you have really met may be granted again the next time that counter moves. The mod saves [b]Profile000.gps.bak[/b] once, before its first ever change.
[*][b]Steam's "Verify integrity of game files" removes the mod[/b] by restoring the stock DLL. Just re-copy the files.
[/list]

[size=5][b]Config[/b][/size]

[code]
ToggleKey   = 0x77      ; virtual-key code that opens the panel (0x77 = F8)
ShowOnStart = 0         ; open the panel as soon as the game starts
AutoShow    = 1         ; open the panel automatically on the Awards screen
SortMode    = 0         ; 0 game order (A-Z), 1 award index, 2 points, 3 completed first, 4 database order
[/code]

Lives in [b]Fear2AwardUnlocker.ini[/b] next to FEAR2.exe, and holds settings only - award state lives in your save, not here.
