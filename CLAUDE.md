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

`tests/test_adopt.cpp` runs the D3D9 hooks under Wine with the Steam overlay's way of hooking played
by the test (build and run commands in its header; `F2AU_ADOPT=1` makes the proxy take the Windows
path under Wine). See "The panel's hooks on Windows".

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
- **On Windows, no function of the mod's in a vtable another hooker reads** (The panel's hooks on
  Windows).

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

What Flash *does* report is `Menu.StateChanged(id)` on every screen change. ActionScript reaches
C++ through a table of 12-byte `{const char* name, void* handler, u8 flag}` rows; the row at
`0x1F6B80`/`0x1F6B84` is that callback (handler RVA `0x94A0`, `__stdcall(void*, GFxValue*, int)`,
reading `args[0]` as type 3 with the double at `+0x08`). Swapping the handler pointer is a clean
data patch. The id of the Awards screen is an ActionScript enum value and is not in the binary, so
the mod learns it from the player instead.

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
hook. Not yet seen in game on Windows.

## Data tooling

`tools/ltar.py` reads `.Arch01` (LTAR v3) archives — verified byte-identical extraction.
`tools/gadb.py` reads `.gamedb` (GADB v6). `tools/gen_awards.py` combines them to emit
`src/awards_table.h`. Both readers were written for this project; the formats are documented in the
module docstrings.

Archive priority: `default.archcfg` lists the search path and **the last entry wins** — proven
because `Loki.gamedb` exists in six archives and only the last-listed `Layer01_v103.Arch01` carries
the DLC3 awards that are live in game.
