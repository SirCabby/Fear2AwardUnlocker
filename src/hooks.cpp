#include "hooks.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "awards.h"
#include "awards_table.h"
#include "crash.h"
#include "callbacks.h"
#include "gameclient.h"
#include "log.h"
#include "mem.h"

namespace f2au::hooks {
namespace {

uintptr_t g_base = 0;

// The originals are __thiscall (this in ecx, args on the stack, callee cleans).
// GCC has no __thiscall, but __fastcall is the same shape once a dummy second
// parameter soaks up edx, and its callee-cleanup matches the `ret 4` / `ret 8`
// the originals use.
using IsCompleteFn = char(__fastcall*)(void* self, void* edx, void* id);
using BuildListFn = int(__fastcall*)(void* self, void* edx, void* a, void* b);
using ResolveProfileFn = void*(__fastcall*)(void* self, void* edx, void* arg);

IsCompleteFn g_orig_is_complete = nullptr;
BuildListFn g_orig_build_list = nullptr;

// IsAchievementComplete is called once per award, in Interface/Achievements
// database order, with `id` pointing into a contiguous array of 0x18-byte
// records. So award identity is just the offset from the first record. We take
// the smallest pointer ever seen as the base: the game sweeps all 58 in order
// at startup, so this is correct from the very first call and self-corrects if
// a stray single query ever arrives first.
uintptr_t g_record_base = 0;

int award_index(void* id) {
  auto p = reinterpret_cast<uintptr_t>(id);
  if (!p) return -1;
  if (!g_record_base || p < g_record_base) g_record_base = p;
  uintptr_t delta = p - g_record_base;
  if (delta % gc::kAchievementRecordStride) return -1;
  int index = static_cast<int>(delta / gc::kAchievementRecordStride);
  return (index >= 0 && index < kAwardCount) ? index : -1;
}

// Reproduce the lookup IsAchievementComplete does internally to reach the
// profile-data object that owns the key/value record vector.
void* resolve_profile_data(void* self) {
  auto owner_ptr = mem::read<uintptr_t>(g_base + gc::kProfileOwnerGlobal);
  if (!owner_ptr) return nullptr;
  auto fn = reinterpret_cast<ResolveProfileFn>(g_base + gc::kResolveProfile);
  void* arg = mem::read<void*>(reinterpret_cast<uintptr_t>(self) + 0x14);
  void* obj = fn(reinterpret_cast<void*>(owner_ptr + gc::kProfileOwnerOffset), nullptr, arg);
  if (!obj) return nullptr;
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(obj) + gc::kProfileDataOffset);
}

// Resolve the profile once we are on the game thread, then push any award lines
// loaded from the config into the profile itself.
void ensure_profile(void* self) {
  if (awards::profile_data()) return;
  void* pd = resolve_profile_data(self);
  if (!pd) return;

  auto b = reinterpret_cast<uintptr_t>(pd);
  auto begin = mem::read<uintptr_t>(b + gc::kRecordsBegin);
  auto end = mem::read<uintptr_t>(b + gc::kRecordsEnd);
  if (!begin || end <= begin || (end - begin) % gc::kRecordStride) {
    logf("profile-data at %p has an unexpected record vector (begin=%p end=%p) - not using it", pd,
         reinterpret_cast<void*>(begin), reinterpret_cast<void*>(end));
    return;
  }
  awards::set_profile_data(pd);
  logf("profile-data=%p, %d records", pd, static_cast<int>((end - begin) / gc::kRecordStride));

  int written = awards::apply_all_forced();
  if (written) logf("wrote %d award(s) from the config into the profile", written);
}

// The game sweeps every award in one go when it (re)builds the awards list.
// Logging the shape of those bursts is how we learn whether opening the Awards
// screen re-queries - which decides both whether toggles refresh live and
// whether a burst can drive auto-show.
DWORD g_sweep_last_tick = 0;
int g_sweep_count = 0;
bool g_sweep_event = false;
DWORD g_install_tick = 0;

// True while the mod is re-pushing award state to the menu: the full sweep that
// causes is ours, not the game rebuilding its list, so it must not be counted.
bool g_refreshing = false;

void note_sweep() {
  if (g_refreshing) return;
  g_sweep_last_tick = GetTickCount();
  ++g_sweep_count;
}

char __fastcall hk_is_complete(void* self, void* edx, void* id) {
  ensure_profile(self);
  note_sweep();

  const int index = award_index(id);
  const awards::Forced f = awards::forced(index);
  if (f == awards::Forced::kOn) return 1;
  if (f == awards::Forced::kOff) return 0;

  const char result = g_orig_is_complete(self, edx, id);
  awards::set_observed(index, result != 0);  // so the panel can show real state
  return result;
}

int __fastcall hk_build_list(void* self, void* edx, void* a, void* b) {
  logf("BuildAchievementList called");
  return g_orig_build_list(self, edx, a, b);
}

using PushFn = void(__cdecl*)(void*);
PushFn g_orig_push = nullptr;

// Menu.StateChanged(newState) - Flash's own notification that the menu screen
// changed. __stdcall, callee cleans 12 bytes.
using StateChangedFn = void(__stdcall*)(void* self, void* args, int argc);
StateChangedFn g_orig_state_changed = nullptr;
int g_menu_state = -1;

void __stdcall hk_state_changed(void* self, void* args, int argc) {
  if (argc >= 1 && args) {
    auto v = reinterpret_cast<uintptr_t>(args);
    if (mem::read<int>(v) == gc::kGfxValueTypeNumber) {
      const int state = static_cast<int>(mem::read<double>(v + gc::kGfxValueNumberOffset));
      if (state != g_menu_state) {
        logf("menu state -> %d", state);
        g_menu_state = state;
      }
    }
  }
  g_orig_state_changed(self, args, argc);
}

void __cdecl hk_push_to_flash(void* movie) {
  logf("PushAchievementsToFlash called - the Awards list is being fed to Flash");
  g_orig_push(movie);
}

// -------------------------------------------------- refreshing the Awards screen
// The game pushes award completion into the menu movie exactly once, while the
// menu is being built, so a toggle made afterwards stayed invisible on the
// game's own Awards screen until the next launch. Calling that push again fixes
// it: it re-reads every award through the hooked vtable slot and re-sends the
// result, and on the ActionScript side the rows read their state back off the
// objects it updates, so no screen rebuild is needed.
using GetActiveMenuFn = void*(__fastcall*)(void* self, void* edx);
using PushProgressFn = void(__cdecl*)(void* menu, void* profile_data);

bool g_can_refresh = false;  // set once the build check below passes

// Fingerprint of everything the mod is forcing. Comparing it each frame catches
// every path that can change an award - the checkboxes, unlock/lock all, clear
// overrides - without each of them having to remember to ask for a refresh.
uint64_t forced_fingerprint() {
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (int i = 0; i < kAwardCount; ++i) {
    h = (h ^ static_cast<uint8_t>(awards::forced(i))) * 1099511628211ull;
  }
  return h;
}

uint64_t g_pushed_fingerprint = 0;

// False when there is no menu movie to push into - during gameplay, or before
// the menu is built - in which case the caller should just try again later.
bool refresh_award_screen() {
  if (!g_can_refresh) return false;

  auto get_menu = reinterpret_cast<GetActiveMenuFn>(g_base + gc::kGetActiveMenu);
  void* menu = get_menu(reinterpret_cast<void*>(g_base + gc::kMenuManagerSingleton), nullptr);
  if (!menu) return false;

  auto push = reinterpret_cast<PushProgressFn>(g_base + gc::kPushAchievementProgress);
  g_refreshing = true;
  push(menu, nullptr);  // second argument is the missions half; unused here
  g_refreshing = false;
  return true;
}

// Confirm the refresh path is the one we reversed before ever calling into it:
// the push's prologue, the menu getter's prologue, and - the telling one - that
// the push's first call really does land on the helper that invokes
// "AchievementClear" on the Flash side.
bool verify_refresh_path() {
  const uint8_t* push = reinterpret_cast<const uint8_t*>(g_base + gc::kPushAchievementProgress);
  static const uint8_t kPushPrologue[] = {0x83, 0xEC, 0x1C, 0x55, 0x8B, 0x6C, 0x24, 0x24, 0x57};
  for (size_t i = 0; i < sizeof(kPushPrologue); ++i) {
    if (push[i] != kPushPrologue[i]) {
      logf("WARNING: award-progress push prologue mismatch at byte %d - screen refresh off",
           static_cast<int>(i));
      return false;
    }
  }

  const uint8_t* getter = reinterpret_cast<const uint8_t*>(g_base + gc::kGetActiveMenu);
  static const uint8_t kGetterPrologue[] = {0x8B, 0x81, 0x40, 0x01, 0x00, 0x00, 0x83, 0xF8, 0x04};
  for (size_t i = 0; i < sizeof(kGetterPrologue); ++i) {
    if (getter[i] != kGetterPrologue[i]) {
      logf("WARNING: menu-getter prologue mismatch at byte %d - screen refresh off",
           static_cast<int>(i));
      return false;
    }
  }

  const uintptr_t site = g_base + gc::kAchievementClearCall;
  if (mem::read<uint8_t>(site) != 0xE8 ||
      site + 5 + mem::read<int32_t>(site + 1) != g_base + gc::kMenuAchievementClear) {
    logf("WARNING: award-progress push does not call the expected helper - screen refresh off");
    return false;
  }
  const uintptr_t name = mem::read<uintptr_t>(g_base + gc::kAchievementClearNameImm);
  if (name != g_base + gc::kAchievementClearNameStr ||
      std::strcmp(reinterpret_cast<const char*>(name), "AchievementClear") != 0) {
    logf("WARNING: the helper does not invoke AchievementClear - screen refresh off");
    return false;
  }
  return true;
}

}  // namespace

void uninstall() {
  if (!g_base) return;

  const uintptr_t vt = g_base + gc::kAchievementVTable;
  if (g_orig_is_complete)
    mem::write<void*>(vt + gc::kSlotIsComplete * 4, reinterpret_cast<void*>(g_orig_is_complete));
  if (g_orig_build_list)
    mem::write<void*>(vt + gc::kSlotBuildList * 4, reinterpret_cast<void*>(g_orig_build_list));

  // Point the relative call back at the real function.
  if (g_orig_push) {
    const uintptr_t site = g_base + gc::kPushAchievementsCallSite;
    if (mem::read<uint8_t>(site) == 0xE8) {
      const int32_t rel =
          static_cast<int32_t>(reinterpret_cast<uintptr_t>(g_orig_push) - (site + 5));
      mem::write<int32_t>(site + 1, rel);
    }
  }

  if (g_orig_state_changed)
    mem::write<void*>(g_base + gc::kMenuStateChangedHandler,
                      reinterpret_cast<void*>(g_orig_state_changed));

  callbacks::uninstall(static_cast<unsigned long>(g_base));

  // Nothing to unpatch for the refresh - it only calls the game - but with the
  // vtable hook gone it would push the game's own answers back over the toggled
  // ones, so stop it here rather than at detach.
  g_can_refresh = false;

  g_orig_is_complete = nullptr;
  g_orig_build_list = nullptr;
  g_orig_push = nullptr;
  g_orig_state_changed = nullptr;
  logf("achievement hooks removed");
}

int menu_state() { return g_menu_state; }

bool consume_sweep_event() {
  if (!g_sweep_event) return false;
  g_sweep_event = false;
  return true;
}

void poll() {
  // Re-push award state to the menu whenever the toggles have moved since the
  // last push. refresh_award_screen() fails while there is no menu movie to push
  // into, so leaving the fingerprint alone until it succeeds means a toggle made
  // mid-game is applied the moment the player reaches the menus.
  const uint64_t fp = forced_fingerprint();
  if (fp != g_pushed_fingerprint && refresh_award_screen()) {
    g_pushed_fingerprint = fp;
    logf("re-pushed award state to the menu - the game's Awards screen now agrees");
  }

  // A sweep is "finished" once no further queries have arrived for a moment.
  if (!g_sweep_count) return;
  if (GetTickCount() - g_sweep_last_tick < 250) return;

  const int n = g_sweep_count;
  g_sweep_count = 0;
  const DWORD since_install = GetTickCount() - g_install_tick;
  logf("award sweep finished: %d queries (%lums after hooks installed)", n, since_install);

  // Only a full sweep means the list was rebuilt; ignore incidental one-offs,
  // and ignore the one the game does while it is still starting up.
  if (n >= kAwardCount / 2 && since_install > 5000) g_sweep_event = true;
}

bool install() {
  HMODULE gc_mod = nullptr;
  for (int i = 0; i < 900 && !gc_mod; ++i) {  // up to ~90s; it loads ~200ms in
    gc_mod = GetModuleHandleA("gameclient.dll");
    if (!gc_mod) Sleep(100);
  }
  if (!gc_mod) {
    logf("ERROR: gameclient.dll never appeared - no hooks installed");
    return false;
  }

  g_base = reinterpret_cast<uintptr_t>(gc_mod);
  logf("gameclient.dll base=%p (preferred %p)", reinterpret_cast<void*>(g_base),
       reinterpret_cast<void*>(gc::kPreferredBase));

  // Confirm this is the build we reversed before patching anything.
  // IsAchievementComplete opens with `mov eax, ds:<abs>` then `sub esp,0x1c`;
  // the absolute operand relocates, so bytes 1-4 are skipped.
  const uint8_t* fn = reinterpret_cast<const uint8_t*>(g_base + gc::kIsAchievementComplete);
  if (!(fn[0] == 0xA1 && fn[5] == 0x83 && fn[6] == 0xEC && fn[7] == 0x1C && fn[8] == 0x85 &&
        fn[9] == 0xC0 && fn[10] == 0x57)) {
    logf("ERROR: IsAchievementComplete prologue mismatch - unknown build, refusing to hook");
    return false;
  }

  const uintptr_t vt = g_base + gc::kAchievementVTable;
  const uintptr_t slot8 = mem::read<uintptr_t>(vt + gc::kSlotIsComplete * 4);
  if (slot8 != g_base + gc::kIsAchievementComplete) {
    logf("ERROR: vtable slot %d is %p, expected %p - refusing to hook", gc::kSlotIsComplete,
         reinterpret_cast<void*>(slot8),
         reinterpret_cast<void*>(g_base + gc::kIsAchievementComplete));
    return false;
  }

  if (awards::settings().disable_achievements) {
    logf("achievement hooks disabled by config");
    return true;
  }

  g_orig_is_complete = reinterpret_cast<IsCompleteFn>(
      mem::hook_vtable(vt, gc::kSlotIsComplete, reinterpret_cast<void*>(&hk_is_complete)));
  g_orig_build_list = reinterpret_cast<BuildListFn>(
      mem::hook_vtable(vt, gc::kSlotBuildList, reinterpret_cast<void*>(&hk_build_list)));

  g_orig_push = reinterpret_cast<PushFn>(mem::hook_call_site(
      g_base + gc::kPushAchievementsCallSite, reinterpret_cast<void*>(&hk_push_to_flash)));
  if (g_orig_push != reinterpret_cast<void*>(g_base + gc::kPushAchievementsToFlash)) {
    logf("WARNING: PushAchievementsToFlash call site resolved to %p, expected %p",
         reinterpret_cast<void*>(g_orig_push),
         reinterpret_cast<void*>(g_base + gc::kPushAchievementsToFlash));
  }

  // Hook the Flash -> C++ menu-state notification by swapping the handler in the
  // callback table, after checking the row really is the one we expect.
  const uintptr_t name_slot = g_base + gc::kMenuStateChangedName;
  const uintptr_t handler_slot = g_base + gc::kMenuStateChangedHandler;
  const uintptr_t name_ptr = mem::read<uintptr_t>(name_slot);
  const uintptr_t handler = mem::read<uintptr_t>(handler_slot);
  if (name_ptr == g_base + gc::kMenuStateChangedNameStr &&
      handler == g_base + gc::kMenuStateChangedExpected) {
    g_orig_state_changed = reinterpret_cast<StateChangedFn>(handler);
    mem::write<void*>(handler_slot, reinterpret_cast<void*>(&hk_state_changed));
    logf("hooked Menu.StateChanged (orig %p)", reinterpret_cast<void*>(handler));
  } else {
    logf("WARNING: Menu.StateChanged table row unexpected (name=%p handler=%p) - "
         "auto-show disabled",
         reinterpret_cast<void*>(name_ptr), reinterpret_cast<void*>(handler));
  }

  // Diagnostic only, and it patches an import table we would then have to
  // restore, so keep it out of normal runs.
  if (awards::settings().trace_menu_callbacks) install_purecall_logger();
  if (!awards::settings().disable_callbacks) callbacks::install(g_base);

  // The Awards screen shows a copy of the state pushed to Flash when the menu was
  // built, so a toggle needs that push repeated. Check the path is the one we
  // reversed before we are ever in a position to call it.
  g_can_refresh = verify_refresh_path();
  g_pushed_fingerprint = forced_fingerprint();
  if (g_can_refresh)
    logf("award-screen refresh available - toggles will update the game's own screen");

  g_install_tick = GetTickCount();
  logf("hooks installed (%d awards, %d of them with no counter that could earn them)",
       kAwardCount, [] {
         int n = 0;
         for (const auto& a : kAwards)
           if (!a.counter) ++n;
         return n;
       }());
  return true;
}

}  // namespace f2au::hooks
