# Fear2AwardUnlocker — project guide

A client-side mod for **F.E.A.R. 2: Project Origin** (Steam appid 16450, 32-bit, LithTech
Jupiter EX 2 / "Loki"), cross-built on Linux with mingw-w64. It adds an in-game panel of per-award
toggles. `README.md` is the user-facing doc; this file records **what was reverse-engineered**, so
none of it has to be rediscovered.

## Build & deploy

```sh
make            # -> build/LTMemory.dll   (config.mk sets GAME_DIR; gitignored)
make install    # rename stock ltmemory.dll -> LTMemory_orig.dll (once), deploy ours atomically
make uninstall  # restore the stock DLL
python3 tools/gen_awards.py --game-dir "$GAME_DIR"          # regenerate src/awards_table.h
python3 tools/gen_awards.py --game-dir "$GAME_DIR" --print   # inspect the award table
```

`tests/test_adopt.cpp` runs the D3D9 hooks under Wine. The test plays the Steam overlay's way of
hooking, Windows' own d3d9 (a vtable per device) and the Windows shim engine (build and run commands
in its header; `F2AU_ADOPT=1` makes the proxy take the Windows path under Wine). See "The panel's
hooks on Windows".

**The installed file must be named `LTMemory.dll`, exactly.** That is the spelling in FEAR2.exe's
import table (`grep -aob 'LTMemory\.dll' FEAR2.exe`), and on a case-sensitive filesystem Wine
finds that literal name first - it only falls back to a case-insensitive scan when the exact name
is missing. Steam ships
the stock DLL lowercase on Linux, so it is easy to end up with both `ltmemory.dll` and
`LTMemory.dll`; when that happens the uppercase one loads and every install written to the
lowercase one is silently ignored. This actually happened: a run showed old behaviour with no sign
of the new code, and the log's missing lines were the giveaway. `make install` now moves the stock
file aside rather than copying it, always writes `LTMemory.dll`, and deletes any of our own copies
under another case. **When a change seems not to have taken effect, check the log for a line only
the new build prints before assuming the code is wrong.**

## ⛔ Rules

- **Never patch `FEAR2.exe` on disk.** SteamStub validates the image and refuses a modified one. A
  proxy DLL plus in-memory hooks is unaffected.
- **Never use absolute addresses.** `gameclient.dll` does *not* get its preferred `0x10000000` base
  (observed at `0x02d80000`). Everything is `module_base + RVA`. Byte signatures must wildcard any
  embedded absolute operand, because those relocate.
- **`kAwards[]` order is load-bearing.** The hook identifies an award by its index in
  `Interface/Achievements` *database* order. Sort by `AwardDef::order` for display only; never
  reorder the array.
- **Back up before touching a profile.** A malformed `.gps` makes the game reset the *entire*
  profile to defaults, not just awards.
- **On Windows, no function of the mod's in a vtable another hooker reads, and never move a device
  off the vtable it has.** Windows' d3d9 gives each device its own vtable and rewrites it, and the shim
  engine finds its hooks by it. See "The panel's hooks on Windows".

## What the game does

### Awards are stored, one byte each

58 awards, records in `Interface/Achievements` of `Database\Loki.gamedb`. **Every one of them,
Reborn DLC included, has a persisted flag in the profile.** The key is

```
crc32("Achievements" + <name of the award's Storage record>)
```

`Storage` is attribute slot 8 of the achievement — the `PCACH_*` / `PC_DLC_ACH_*` record in
`Interface/Collectables` that also serves as the platform id. `"Achievements"` is a literal
`const char*` at `gameclient.dll` RVA `0x1F4BB0`, pasted in front of the record name by the helper
at `0x82290` (`strncpy_s` + `strncat_s` into a 64-byte buffer) before `0x1723F7` CRC-32s it.

`IsAchievementComplete` (`0x16D7B0`) reads that byte and `GrantAchievement` (`0x16D6D0`) writes it,
both via the generic profile accessors `0x82C60` (get) / `0x82A40` (set), handed a 20-byte key
descriptor `{ type=1, storageRecord, mission=-1, level=-1, 0 }`. Type 1 in `0x82430`'s jump table
(arm at `0x824E4`, builder `0x82360`) is the "prefix + record name" form; type 2 is the
`"%sM%dL%d"` mission/level form.

The `Conditions` struct (slot 6) — a counter record in `Interface/GameProfileData` plus a
threshold — is what makes the game *grant* an award during play. It is **not** what the Awards
screen reads back. Nine Reborn DLC awards have a null counter and threshold 0, which is why the game
never grants them on PC; it has nowhere to *earn* them, not nowhere to *keep* them.

An earlier note here claimed there was no persisted flag, on the strength of a CRC-32 brute force
over 282k candidate strings that found no hit for any `ACH_*`/`PCACH_*` name. That search was simply
missing the `"Achievements"` prefix. Verified against a real profile: all 58 keys are present, and
the 30 that are set are exactly the 30 the game reported in `Example.png` ("30 of 58 awards
complete"), award for award.

Two awards (`ACH_Collect_Journals_100`, `ACH_Collect_SlowMo`) are earned from `CollectableList`
aggregate counters computed from individual collected-item flags, so a naive "counter >= threshold"
model mispredicts when the game will grant them. Their stored flag behaves like any other.

### The profile file

`Documents\WBGames\FEAR2\<steamid>\Profiles\Profile000.gps` — no checksum, no signature.

```
u32 version(17)  u32 version(17)  u32 recordCount
recordCount x { u32 crc32(name), u32 valueLen, <valueLen bytes LE> }   # len 1 = bool, 4 = int32
```

Both version fields are validated; a mismatch resets the profile. Keys are plain zlib CRC-32 of the
record name, and for mission/level-scoped records the name is first formatted `"%sM%dL%d"`
(`gameclient.dll` `MakeKey` at RVA `0x0822F0`; the unscoped `prefix + recordName` builder is
`0x082360`, which formats via `0x082290`; the CRC-32 itself is `0x1723F7`).

In memory, the same records live in a vector on a profile-data object: `+0x18` begin, `+0x1C` end,
20-byte stride, value at `+0x00`, update policy at `+0x08` (0 keep-max, 1 keep-min, 2 overwrite),
key at `+0x10` (`FindRecordIndex`, RVA `0x0825E0`). **Write a flag record as one byte** — the three
bytes above it are the game's.

### gameclient.dll offsets (RVAs against the preferred base 0x10000000)

All verified against the shipped binary *and* re-checked in live process memory. See
`src/gameclient.h`.

| what | RVA |
| --- | --- |
| achievement manager vtable | `0x1C9738` |
| profile get / set by key descriptor | `0x82C60` / `0x82A40` |
| key descriptor -> crc32 | `0x82430` (dispatch), `0x82290` (prefix+name), `0x822F0` (`%sM%dL%d`) |
| `"Achievements"` key prefix | `0x1F4BB0` |
| `Storage` link lookup, `GetRecordLink(GetAttribute(rec,"Storage"), i)` | `0x11D5E0` |
| slot 2 `BuildAchievementList` | `0x16E050` |
| slot 7 `GrantAchievement` | `0x16D6D0` |
| slot 8 `IsAchievementComplete` | `0x16D7B0` |
| `g_pAchievementMgr` | `0x1F64B0` |
| profile-owner global | `0x1F9E30` (+`0x98`) |
| resolve-profile call | `0x51FE0`, then `+0x86C` |
| `UnlockAchievements` cvar handle | `0x1F9E8C` |

`IsAchievementComplete` is called **once per award, in database order**, with a pointer into a
contiguous array of `0x18`-byte records — which is how the mod maps a call to an award (smallest
pointer seen = index 0).

### Console variables

`UnlockAchievements` and `CheatAchievements` are real console variables, settable from
`autoexec.cfg` (registration reads before it writes, so a pre-set value survives).
`UnlockAchievements 1` short-circuits `IsAchievementComplete` to true for *every* award — the
comparison is a strict `== 1.0f`, so only exactly `1` works. It is **display-only**: it is read at
the top of `IsAchievementComplete` and nowhere in the grant path, so nothing is ever written.
(`GrantAchievement`'s own early-out is on `[mgr+0x18]`, a flag the constructor zeroes, not on this
cvar.) `CheatAchievements` has no read sites anywhere and appears to be dead code.

### Localization: call the game, do not parse Loki.strdb

`BuildAchievementList` turns a loc key into display text with
`mgr = [0x20459C]; ctx = [0x2045A0]; text = mgr->vtbl[8](ctx, key)` (vtable byte offset 0x20),
returning a wide string; missing keys come back as `""` or `"["`. `src/loc.cpp` calls exactly that,
so names and descriptions are always right and always in the player's language.

Parsing `Loki.strdb` directly was tried and abandoned: the SKDB v5 key block (ASCII, 4-byte aligned,
6193 entries ending at 0x19B1C) and the UTF-16 value block that follows are **not** index-parallel,
and the mapping table is in a later section that was not worth reversing once the engine call was
found.

### Refreshing the game's own Awards screen

The screen renders a copy of what C++ pushed to Flash, and the push happens once, during menu
construction. **Re-running that push is what makes a toggle show up without a restart.** Two
functions feed it, and only the second carries state:

| RVA | what it sends |
| --- | --- |
| `0xA470` `PushAchievementsToFlash(menu)` | `Menu.AddAchievementInfo` per award: `AchievementIndex`, `AchievementName`, `AchievementDescription`, `AchievementPoints`. Skips awards whose `AchievementHide` attribute is set. **No completion state.** |
| `0xA130` `PushAchievementProgress(menu, profileData)` | one `Menu.AchievementClear`, then `Menu.AchievementSetProgress(id, progress, required)` per award, in database order |

`0xA130` is `__cdecl` and **ignores its second argument** (checked instruction by instruction: no
read of `[esp+arg1]` anywhere in `0xA130`-`0xA2A6`); the game passes it only because the same
wrapper also refreshes missions. It reads completion as
`([player+0x828])->vtbl[8](player+0x828, record)` — the achievement manager is *embedded* at
`+0x828` on the profile owner, its vptr is the same `0x1C9738`, and slot 8 is
`IsAchievementComplete`. So the mod's existing vtable hook is already in that path — which is also
where the boot-time sweep of 58 queries comes from — and re-calling `0xA130` re-pushes the toggled
values with no new hook.

Call graph: `0xA130` <- `0xA2B0(menu, profileData)` (which also does `ClearUnlockedMissions` +
missions) <- three places: `CActionScriptMenu::Construct` at `0xB2A7`, and slots 1 and 4 of the
listener vtable at `0x1C2C30` (`0xB370`, `0xAFC0`). That the game itself re-pushes from event
callbacks is what says re-pushing at an arbitrary time is a supported operation.

The menu object to pass is not a global: `0xAFC0` gets it with
`GetActiveMenu(this = 0x1FD960)` at `0x2FAF0` (`__thiscall`, returns null when no menu movie is
up) and does nothing if it is null. The mod does exactly the same, and holds the request until a
menu exists, so a toggle made mid-game lands as soon as the player reaches the menus.

On the ActionScript side (`interface\mainmenu.bndl`, AS2 — the `.gfx` movies are inside the
`.bndl` bundles, not loose):

- `LokiMenuEvents.AddAchievementInfo / AchievementClear / AchievementSetProgress` forward to
  `Globals._AchievementMgr`, a **persistent** `MenuLayers.CAchievementMgr` holding one
  `CAchievementInfo` per award.
- `AchievementClear` -> `ClearAchievements()` resets each entry; it does **not** empty the array.
  (Proof: the boot order is `AddAchievementInfo` x58, *then* `AchievementClear`.)
- `MenuLayers.CAchievements.OnTransitionTo` -> `_AchievementMgr.PopulateTable(m_table)` +
  `UpdateView`, and `CAchievementTableSettings.DisplayItem` re-reads `m_nProgress`, `m_nRequired`
  and `GetFlashFrame()` (`"complete"` / `"incomplete"`) off those same objects on every repaint.
  So a re-push does not need a screen rebuild; the rows pick it up when the table next paints,
  which is guaranteed on the next transition to the screen.

### Menu screen detection

There is **no C++ event when the Awards screen opens.** `BuildAchievementList`,
`PushAchievementsToFlash`, `AchievementClear`, `AddAchievementInfo` and `AchievementSetProgress`
each have exactly one caller and all run once during menu construction at boot; opening the screen
produces no achievement traffic at all (verified at runtime). So a sweep of `IsAchievementComplete`
cannot be used as a screen signal.

ActionScript reaches C++ through a table of 12-byte `{const char* name, void* handler, u8 flag}`
rows (`src/callbacks_table.h`, all 163 hooked). Each handler is `__stdcall(void*, GFxValue*, int)`;
a GFx value has its type tag at `+0x00` (3 = number, 4 = string) and the payload at `+0x08`.
`Menu.StateChanged` (row `0x1F6B80`/`0x1F6B84`, handler `0x94A0`) is **not** a screen signal. The
ActionScript defines a wrapper for it but nothing calls it, and no log has ever had a `menu state ->`
line. Its hook only logs.

**What the menu's ActionScript does.** This was read from `interface\menu\main.gfx` (AS2) in the
latest bundle, `_postrelease\pc\interface\mainmenu_titleupdate.bndl` (`Layer01_v103.Arch01`). The
base, v101 and `interface\_postrelease\pc\mainmenu_v102.bndl` copies agree on everything below. The
title update adds `Menu.IsDLCCorrupt` and `UpdatePrompts` (`IsConnectedOnline`) to
`CMainMenu.InitComponents`, run once while the menu is built. `tools/menu_as.py --list` shows the
bundles; `tools/menu_as.py <bundle> <movie>` disassembles one.

- **The PC main menu** (`MenuLayers.CMainMenu`):
  - New Game → layer `NEWGAMEMENU` 10.
  - Continue → `cb_LoadLastSave` → `SaveGame.LoadCheckpointSave`.
  - Options → `OPTIONS` 2.
  - Achievements → `ACHIEVEMENTS` 28.
  - Quit → `cb_QuitGame` → `Global.Exit`.
  - A Load button (`LOADMENU` 23) is defined `AS_NONE`, and no layer class exists for it.
- **Other layer ids** (`Enum.LayerId`): `MAIN_MENU` 1, `GAMMA` 27 (the first-run brightness screen).
- **Selecting.** `CButton.HandleSelect` runs the button's callback first (`cb_PushLayer` →
  `Globals._MM.PushLayer(id)`). Only then does it call `PlaySelectSound`: the key `'SELECT'` becomes
  `MENU_SELECT`, i.e. `Global.PlaySound("menu_select")`.
- **The screen change** (`MenuManagers.CMainMenu`):
  - `StartTransition` calls the old layer's `OnTransitionFrom` and the new one's `OnTransitionTo` at
    once, before the select sound.
  - Then comes a fade on the game clock: `FADE_OUT_TIME` 300 ms, then one frame. (The fade-in divides
    by `FADE_TOTAL_TIME` from a start time it has just reset to 0, so it ends at once.)
  - Only then do the new layer's `OnActivate` and, from the next frame, its `OnUpdate` run.
- **`Menu.SetReturnMenu(this._LayerId)`** is called in `OnActivate` by the hub layers: CMainMenu (1),
  CNewGameMenu (10), CCustomWorldMenu (18), CMPLanPlay (11), CMPInternetPlay (14) and CMPRankedPlay
  (12). The game keeps it at `gameclient+0x1FDD44` (handler `0x95A0`).
- **Leaving.** `PopLayer` is the only place `menu_back` is played, and only when the stack is deeper
  than one.
- **Every frame**, the root `onEnterFrame` → `LokiMenuEvents.OnUpdate` calls
  `Global.GetAccumulatedTime` once. It also calls `Menu.CancelAttractMode`, but only while a message
  box is up or on the gamma screen.
- **Every key press** calls `Menu.CancelAttractMode` once per text box in the menus, right after the
  menu's own handling of the key. Each `MenuComponents.CInput` registers a key listener; the laptop
  trace showed 11 calls per press. So picking Awards with Enter is followed, in the same frame, by 11
  of them.
- **What other screens call:**
  - Options (`COptionsMenu.OnUpdate`) calls `Menu.CancelAttractMode` every frame.
  - The Awards layer (`CAchievements`) calls nothing. Its `OnTransitionTo` sets the title through
    Scaleform's translator and runs `PopulateTable`; its `OnUpdate` is `UpdateView`.
- **`SaveGame.GetCheckpointSaveData`** is called by `CMainMenu.InitComponents` (once, while the menu is
  built), by `CSPMenu.OnTransitionFrom`, and by `CMissionSelect.OnUpdate` every frame.

**The inference** (`callbacks.cpp`):

1. `SetReturnMenu(1)` puts it on the main menu; any other id means another screen.
2. A `menu_select` there counts as a candidate only if no other call came since the last menu frame.
   That rules out Continue, New Game and Quit, whose callbacks call the game first.
3. If the fade and then eight menu frames (counted by `GetAccumulatedTime`) pass with no call into the
   game, it is the Awards screen. That rules out New Game (`SetReturnMenu(10)` at `OnActivate`).
4. `Menu.CancelAttractMode` counts as a call only when it comes in three frames running. Options'
   `OnUpdate` does that; a key press, whose calls all land in one frame, does not.
5. A selection after which the menu stops running for 3 s (a level loading) is dropped.
6. `menu_back` leaves the Awards screen.

Until 2026-10-22 the main menu was marked by `GetCheckpointSaveData`, and it needed 350 ms of quiet
after the select. That failed on the first Windows run, which went through the brightness screen:
confirming that screen used the inference up before the main menu appeared, so Awards was never
detected. The 350 ms is also shorter than the fade plus a frame at 30 fps, which let Options pass.
Then, on the laptop, the key-press `CancelAttractMode` burst cancelled every keyboard pick.
`TraceMenuCallbacks = 1` found it; mouse picks make no burst.

`tests/test_menu_nav.cpp` plays these sequences against the real state machine, keyboard included.

### The Awards screen is Scaleform, and the channel is one-way

The menu is a Flash (AS2) movie, `interface\menu\modules\menu_achievements.gfx`. C++ pushes rows in
via `Menu.AddAchievementInfo`, but the AS side has no callback into C++ for achievements —
`cb_SelAchievement` is internal to ActionScript. So **native per-row toggles are not possible**
without replacing `main.gfx`, which is why the mod draws its own ImGui panel instead.

## Shutdown: load the allocator early, and unhook before the game tears down

An early build made the game show **R6025 "pure virtual function call"** on exit. A/B against the
stock DLL confirmed it was the mod (stock exits clean), and no such bug is reported for this game
anywhere. Two causes, fixed together:

1. **`LTMemory_orig.dll` was loaded lazily**, on the first `GetILTMemory` call. That made it the
   most recently initialised module, so the loader unloaded it *first* - while the game still had
   destructors freeing memory through the `ILTMemory` interface, whose vtable lives in that DLL.
   Calling a virtual on the by-then destructed allocator is precisely a pure virtual call. It is now
   loaded from `DLL_PROCESS_ATTACH` (and pinned), which puts it back where the stock game has it.
   Loading under the loader lock is normally discouraged, but this DLL only imports KERNEL32 and
   MSVCR80, and the stock game links it statically anyway.
2. **Nothing was ever unhooked.** The window procedure, D3D9 vtable, achievement vtable, patched
   call site and all 162 callback thunks still pointed into the DLL after it unmapped. Everything is
   now removed at `Global.Exit` - while the game is still healthy - and again at detach as a
   backstop.

Debugging notes for next time: R6025 does **not** raise an SEH exception (the CRT calls `_purecall`
and aborts), so `SetUnhandledExceptionFilter` never sees it. Every game module imports `_purecall`
from MSVCR80, so it can be IAT-hooked to log the caller - `TraceMenuCallbacks = 1` enables that,
along with logging every ActionScript bridge call. Both are off by default because the `_purecall`
hook patches an import table the mod would then have to restore.

`Disable = overlay,callbacks,achievements` in the ini turns whole subsystems off, which is how the
above was bisected.

## The panel's hooks on Windows (src/adopt.cpp, overlay.cpp)
Found with Fear3CabbyCodes on 2026-09-27 (its `CLAUDE.md`, "The panel's hooks on Windows", has the
whole account, from the Steam overlay's log and `gameoverlayrenderer.dll`'s code). The overlay wraps
`IDirect3D9` in an object of its own (`IWrapIDirect3D9`) and, inside its `CreateDevice`, hooks the new
device by writing a jump into whatever function each of these device slots points to at that moment:
Release 2, CreateAdditionalSwapChain 13, GetSwapChain 14, Reset 16, Present 17, SetGammaRamp 21,
SetRenderState 57, SetSamplerState 69, PresentEx 121, ResetEx 132 - keeping ONE saved original per
hook and skipping a slot that is its own detour already. EndScene is not among them; Reset is. With
the mod's Reset in D3D9's device class vtable (1.0.0), a second `CreateDevice` would take it for the
overlay's original while the mod's own way on is D3D9's Reset, by then the overlay's detour: the two
would call each other at the next Reset until the stack ran out. Whether F.E.A.R. 2 ever makes a
second device is not known (the log names only the first), so this is a risk, not a crash seen.
Hence, on Windows only (`adopt::enabled()`: not Wine, or `F2AU_ADOPT` in the environment), the device
the game makes for **its window** (a window of this process made by the thread that loaded the mod -
`adopt::init` runs in DllMain for that, not on the mod thread) gets a private copy of its vtable
(192 slots) with the mod's EndScene and Reset, and the hooks call on through the vtable the device
had, as it is at the time; the class's vtable is left alone. `IDirect3D9::CreateDevice` stays hooked
in place (the overlay's wrapper object, whose vtable it never reads again). `on_game_exiting()` and
the detach backstop put the class's functions back into the copy (`adopt::unhook_all`) instead of
writing a vtable through `g_device`. Under Wine the class vtable is hooked as before. The shutdown
above is unchanged and exits clean under Wine in every case (`hooks removed cleanly`), unlike the
Fear3 mods' teardown (which never finished there). `tests/test_adopt.cpp` plays the overlay: 1.0.0
loops at a second device's Reset (`LOOP`); the fix passes, and the Wine path still takes the class
hook. The copy was first run in game on Windows on 2026-10-22, and it crashed the game at start. The
next section explains why, and what replaced the copy for Windows' own d3d9.

### A device keeps its vtable: Windows' d3d9 and the shim engine (2026-10-22)

1.0.1 crashed the game at its first start on Windows. The machine was the laptop: Windows 11 build
26200, Intel UHD 620 + MX150, Steam overlay on.

- **The mod's log** ended normally ("BuildAchievementList called"). The give-away was earlier:
  "the class's at 0aa1929c (no module)" - the device's vtable was on the heap.
- **WER's dump** showed an access violation reading 0x10 at `FEAR2.exe+0x218DEA`, on the main
  thread. It came 2.1 s after the device was adopted, before `PushAchievementsToFlash`.
- **The faulting code** is `cmp dword [esi+10h],0` with `this` null, in an engine class wrapping an
  `ID3DXEffect` at `+0x10`. The functions beside it call the effect's `OnResetDevice`/`OnLostDevice`
  (vtable +0x118/+0x114). No frame of the mod's was on the stack.

Two things on Windows rely on a device keeping the vtable it has.

**Windows' d3d9.dll gives each device a vtable of its own.** This is the 32-bit d3d9.dll of build
26200 (`SysWOW64`, hash 7678a845).

- As the device is made (0x1008CA0C), d3d9 saves the class's vtable at `+0x2F58`, copies its 175
  slots to `+0x2F5C` and points the device there.
- d3d9 keeps rewriting that table. `BeginStateBlock` (slot 60, 0x10124020) and a failed Reset
  (0x100E156D) copy the class's first 119 slots back over it.
- A fast-path patcher (0x1007D851) writes slots 49, 57, 65, 67, 69, 89, 92, 94, 96, 98, 100, 102,
  104, 107, 109, 111 and 113.
- D3DX records state blocks through the device's vtable (d3dx9_40.dll: `BeginStateBlock`,
  `SetViewport`, `EndStateBlock`).

**The Windows shim engine knows a COM object by its vtable.** On its first launch on the laptop,
FEAR2.exe got apphelp.dll's `DWM8And16BitMitigation`. On the second launch apphelp.dll was not loaded
at all. Why is not known; it is not a setting on the laptop, whose only layer for FEAR2.exe is
`HIGHDPIAWARE`. So a player may or may not have it.

- The shim hooks the `Direct3DCreate9(Ex)` imports and `IDirect3D9(Ex)::CreateDevice(Ex)`.
- On a device it hooks `CreateAdditionalSwapChain` 13 and `Reset` 16, where a 16-bit fullscreen back
  buffer becomes X8R8G8B8. On an Ex device it also hooks `ResetEx` 132.
- On every object it hooks, it hooks `QueryInterface` 0 and `Release` 2 as well.
- A slot is hooked in place, in the vtable the object has (0x10083D62 records vtable, slot, original
  and hook). Each hook finds its original by the vtable the object points to at the call
  (0x10083A9C).
- When it finds nothing, `QueryInterface` returns E_NOINTERFACE, `Release` returns 0 and releases
  nothing, and `Reset` and `CreateAdditionalSwapChain` call address 0.
- The Steam overlay, asking GetProcAddress for `Direct3DCreate9`, gets apphelp's hook ("returned
  wrong module ... proceeding anyway") and detours that.

So adoption broke the device twice over:

- the device ran on a stale copy of a table that d3d9 switches implementations in;
- four shim hooks could no longer find their originals.

Which of these the engine tripped over is not proven. Its code is encrypted on disk, and a minidump
holds only 256 bytes of it. Both are cured by the device keeping its vtable. The rule is in
`overlay.cpp`, "a vtable the device keeps":

- **A device whose vtable is in no module (its own)** is hooked where it is: EndScene, Reset and
  BeginStateBlock. After BeginStateBlock or Reset, a slot that changed during the call gets the mod's
  function back, calling on to what is there now. The table is this device's alone, so the overlay's
  pass over another device never sees the mod's functions.
- **A class's vtable whose QueryInterface or Release another module has hooked in place** (the shim
  engine's mark) is hooked where it is too. Around each later `CreateDevice` the mod's functions come
  out of it, so the overlay's pass does not find them.
- **Any other class's vtable** is adopted as before.

`tests/test_adopt.cpp` plays both (`windows-d3d9`, `shim`). With the shim, 1.0.1 fails exactly as on
the laptop: E_NOINTERFACE, Release 0, a failed Reset. The fix passes every mode.

FEAR2.exe's code is SteamStub 2.0 (`.bind`). The header at VA 0x776204 is 0xD9 dwords, under a rolling
XOR from key 0x5DA7F69A. It holds no AES key, so the code section cannot be decrypted from it alone.
For the engine's code, take a full dump (WER `LocalDumps\FEAR2.exe`, DumpType 2) or read a running
process.

## Data tooling

`tools/ltar.py` reads `.Arch01` (LTAR v3) archives — verified byte-identical extraction.
`tools/gadb.py` reads `.gamedb` (GADB v6). `tools/gen_awards.py` combines them to emit
`src/awards_table.h`. Both readers were written for this project; the formats are documented in the
module docstrings. `tools/menu_as.py` reads the `.bndl` bundles (BNDL v3) the menus come in and
disassembles a movie's ActionScript (AS2: constant pools resolved, function bodies nested). That
listing is how "What the menu's ActionScript does" above was worked out.

Archive priority: `default.archcfg` lists the search path and **the last entry wins** — proven
because `Loki.gamedb` exists in six archives and only the last-listed `Layer01_v103.Arch01` carries
the DLC3 awards that are live in game.
