#include "callbacks.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "awards.h"
#include "callbacks_table.h"
#include "log.h"
#include "mem.h"
#include "overlay.h"

namespace f2au::callbacks {
namespace {

// Each hooked callback gets a 20-byte trampoline built at runtime:
//
//     pushad                       ; leave every register exactly as we found it
//     push  [esp+0x2C]             ; argc   (the handler's 3rd argument)
//     push  [esp+0x2C]             ; args   (the handler's 2nd argument)
//     push  <index>
//     call  on_callback            ; __cdecl(index, args, argc)
//     add   esp, 12
//     popad
//     jmp   <original handler>     ; tail jump, so the stack the handler sees is
//                                  ; byte-for-byte what the caller set up
//
// After pushad the handler's arguments sit at [esp+36], [esp+40] and [esp+44];
// each push shifts the frame by 4, which is why both re-pushes read [esp+0x2C].
//
// Tail-jumping rather than calling matters: these handlers are __stdcall and
// clean their own arguments, and the count differs between them. By restoring
// esp before the jump we never need to know how many arguments each one takes.
constexpr int kThunkSize = 32;

uint8_t* g_thunks = nullptr;
void* g_original[kMenuCallbackCount] = {};
unsigned g_hits[kMenuCallbackCount] = {};
const char* g_last = nullptr;
int g_installed = 0;
DWORD g_install_tick = 0;

// ---------------------------------------------------------------------------
// Working out when the player is on the Awards screen.
//
// F.E.A.R. 2's Awards screen is pure ActionScript: opening it runs no game code
// at all, so there is nothing to hook. What the bridge *does* show, measured
// live, is:
//
//   main menu appears : SaveGame.GetCheckpointSaveData + Menu.IsDLCCorrupt
//                       (and Menu.SetReturnMenu / PlayVideoLooping)
//   pick a menu item  : Global.PlaySound("menu_select")
//   go back           : Global.PlaySound("menu_back")
//
// Every other main-menu destination announces itself - Options, Multiplayer,
// New Game and Load all call into C++ as they open. Awards is the one that
// stays silent. So: a "menu_select" from the main menu that is followed by
// nothing else means the player opened Awards.
//
// This is inference, not a signal from the game, and it is deliberately
// conservative: anything else calling in cancels it, and returning to the main
// menu always clears it.
enum class Nav { kMainMenu, kPending, kAwards, kOtherScreen };
Nav g_nav = Nav::kMainMenu;
DWORD g_pending_tick = 0;
constexpr DWORD kSilenceMs = 350;

bool is_sound(int index) {
  return !std::strcmp(kMenuCallbacks[index].name, "Global.PlaySound");
}
bool is_timer(int index) {
  return !std::strcmp(kMenuCallbacks[index].name, "Global.GetAccumulatedTime");
}
bool is_main_menu_marker(int index) {
  const char* n = kMenuCallbacks[index].name;
  return !std::strcmp(n, "SaveGame.GetCheckpointSaveData") || !std::strcmp(n, "Menu.IsDLCCorrupt");
}

const char* sound_name(const void* args, int argc) {
  if (!args || argc <= 0) return nullptr;
  auto p = reinterpret_cast<uintptr_t>(args);
  if (IsBadReadPtr(reinterpret_cast<const void*>(p), 16)) return nullptr;
  auto sp = mem::read<uintptr_t>(p + 8);
  if (!sp || IsBadStringPtrA(reinterpret_cast<LPCSTR>(sp), 64)) return nullptr;
  return reinterpret_cast<const char*>(sp);
}

void note_navigation(int index, const void* args, int argc) {
  if (is_timer(index)) return;

  // The game says so itself before it starts tearing the renderer down.
  if (!std::strcmp(kMenuCallbacks[index].name, "Global.Exit")) {
    overlay::on_game_exiting();
    return;
  }

  if (is_main_menu_marker(index)) {
    g_nav = Nav::kMainMenu;
    return;
  }

  if (is_sound(index)) {
    const char* s = sound_name(args, argc);
    if (!s) return;
    if (!_stricmp(s, "menu_select")) {
      // Only a selection made *from* the main menu can be the Awards button.
      if (g_nav == Nav::kMainMenu) {
        g_nav = Nav::kPending;
        g_pending_tick = GetTickCount();
      }
    } else if (!_stricmp(s, "menu_back")) {
      g_nav = Nav::kMainMenu;
    }
    return;
  }

  // Any other call means the screen being opened identified itself, so it is
  // not the Awards screen.
  if (g_nav == Nav::kPending) g_nav = Nav::kOtherScreen;
}

// Best-effort decode of a Scaleform value: a 4-byte type tag, then the payload
// at +8 (double for numbers, pointer for strings). The stride between values is
// not known for certain, so only the first argument is reported.
void describe_args(char* out, size_t n, const void* args, int argc) {
  out[0] = '\0';
  if (!args || argc <= 0) return;
  auto p = reinterpret_cast<uintptr_t>(args);
  if (IsBadReadPtr(reinterpret_cast<const void*>(p), 16)) return;

  const int type = mem::read<int>(p);
  if (type == 3) {
    std::snprintf(out, n, " arg0=number %.10g", mem::read<double>(p + 8));
  } else {
    auto sp = mem::read<uintptr_t>(p + 8);
    if (sp && !IsBadStringPtrA(reinterpret_cast<LPCSTR>(sp), 128))
      std::snprintf(out, n, " arg0=type%d str=\"%.80s\"", type, reinterpret_cast<const char*>(sp));
    else
      std::snprintf(out, n, " arg0=type%d raw=%08X", type, static_cast<unsigned>(mem::read<uint32_t>(p + 8)));
  }
  out[n - 1] = '\0';
}

void __cdecl on_callback(int index, const void* args, int argc) {
  if (index < 0 || index >= kMenuCallbackCount) return;
  g_last = kMenuCallbacks[index].name;
  ++g_hits[index];
  note_navigation(index, args, argc);
  // Menu construction fires most of the table in one burst at startup; that is
  // noise. Once things have settled, every call is interesting, because it is
  // the player navigating.
  // Global.GetAccumulatedTime is a ~30Hz frame timer; logging it buries
  // everything else and grows the log without bound.
  if (!awards::settings().trace_menu_callbacks) return;
  if (is_timer(index)) return;
  const bool settled = GetTickCount() - g_install_tick > 15000;
  if (g_hits[index] == 1 || settled) {
    char desc[160];
    describe_args(desc, sizeof(desc), args, argc);
    logf("menu callback: %s argc=%d%s", kMenuCallbacks[index].name, argc, desc);
  }
}

void write_thunk(uint8_t* t, int index, void* original) {
  const auto at = reinterpret_cast<uintptr_t>(t);
  int o = 0;
  t[o++] = 0x60;  // pushad
  // push dword [esp+0x2C]  (argc, then args - each push shifts the frame by 4)
  t[o++] = 0xFF; t[o++] = 0x74; t[o++] = 0x24; t[o++] = 0x2C;
  t[o++] = 0xFF; t[o++] = 0x74; t[o++] = 0x24; t[o++] = 0x2C;
  t[o++] = 0x68;  // push imm32 (index)
  std::memcpy(t + o, &index, 4);
  o += 4;
  t[o++] = 0xE8;  // call rel32
  const int32_t call_rel =
      static_cast<int32_t>(reinterpret_cast<uintptr_t>(&on_callback) - (at + o + 4));
  std::memcpy(t + o, &call_rel, 4);
  o += 4;
  t[o++] = 0x83;  // add esp, 12
  t[o++] = 0xC4;
  t[o++] = 0x0C;
  t[o++] = 0x61;  // popad
  t[o++] = 0xE9;  // jmp rel32
  const int32_t jmp_rel =
      static_cast<int32_t>(reinterpret_cast<uintptr_t>(original) - (at + o + 4));
  std::memcpy(t + o, &jmp_rel, 4);
}

}  // namespace

void uninstall(unsigned long base_value) {
  const auto base = static_cast<uintptr_t>(base_value);
  int restored = 0;
  for (int i = 0; i < kMenuCallbackCount; ++i) {
    if (!g_original[i]) continue;
    // Only undo rows that still point at our thunk; something else may have
    // taken over in the meantime.
    const uintptr_t slot = base + kMenuCallbacks[i].handler_slot;
    if (mem::read<uintptr_t>(slot) != reinterpret_cast<uintptr_t>(g_thunks + i * kThunkSize))
      continue;
    if (mem::write<void*>(slot, g_original[i])) ++restored;
    g_original[i] = nullptr;
  }
  if (restored) logf("restored %d menu callback handlers", restored);
}

const char* last() { return g_last; }

bool on_awards_screen() { return g_nav == Nav::kAwards; }

void poll() {
  if (g_nav == Nav::kPending && GetTickCount() - g_pending_tick >= kSilenceMs) {
    g_nav = Nav::kAwards;
    logf("no menu code ran after the selection - treating this as the Awards screen");
  }
}

bool install(unsigned long base_value) {
  const auto base = static_cast<uintptr_t>(base_value);
  g_thunks = static_cast<uint8_t*>(VirtualAlloc(
      nullptr, kMenuCallbackCount * kThunkSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!g_thunks) {
    logf("ERROR: could not allocate menu-callback thunks");
    return false;
  }

  g_install_tick = GetTickCount();
  int skipped = 0;
  for (int i = 0; i < kMenuCallbackCount; ++i) {
    const MenuCallback& cb = kMenuCallbacks[i];
    // Only touch a row that still looks exactly as the generator described it.
    if (mem::read<uintptr_t>(base + cb.name_slot) != base + cb.name_str ||
        mem::read<uintptr_t>(base + cb.handler_slot) != base + cb.handler) {
      ++skipped;
      continue;
    }
    void* original = reinterpret_cast<void*>(base + cb.handler);
    g_original[i] = original;
    uint8_t* thunk = g_thunks + i * kThunkSize;
    write_thunk(thunk, i, original);
    if (mem::write<void*>(base + cb.handler_slot, thunk)) ++g_installed;
  }
  FlushInstructionCache(GetCurrentProcess(), g_thunks, kMenuCallbackCount * kThunkSize);
  logf("menu-callback tracing: hooked %d of %d callbacks (%d rows did not match)", g_installed,
       kMenuCallbackCount, skipped);
  return g_installed > 0;
}

}  // namespace f2au::callbacks
