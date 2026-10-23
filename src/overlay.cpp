// In-game overlay: a Dear ImGui panel listing every award with a toggle.
//
// F.E.A.R. 2's own Awards screen is a Scaleform (Flash) movie and the C++ -> AS
// channel is one-way, so there is no supported way to add a clickable control to
// the game's own rows. Drawing our own panel over the top is the practical
// equivalent, and is what the Blasphemous sibling mod does too.
//
// Capture path: FEAR2.exe statically imports Direct3DCreate9, so we patch that
// one import-table slot, then walk COM vtables from the object it returns:
//   Direct3DCreate9 -> IDirect3D9::CreateDevice (slot 16, in place: with the
//                      Steam overlay that object is the overlay's own wrapper)
//                   -> IDirect3DDevice9::Reset (16) / EndScene (42) - on Windows
//                      where the device's vtable is when Windows' d3d9 gave the
//                      device one of its own ("a vtable the device keeps"
//                      below), else in a copy of the class's that is the game's
//                      device's own (adopt.cpp: the overlay hooks what the
//                      device class's Reset points to, at every CreateDevice);
//                      under Wine in the class's vtable.

#include "overlay.h"

#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>

#include "adopt.h"
#include "awards.h"
#include "awards_table.h"
#include "callbacks.h"
#include "crash.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "hooks.h"
#include "loc.h"
#include "log.h"
#include "mem.h"
#include "version.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace f2au::overlay {
namespace {

using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
using CreateDeviceFn = HRESULT(__stdcall*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                           D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using EndSceneFn = HRESULT(__stdcall*)(IDirect3DDevice9*);
using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using BeginStateBlockFn = HRESULT(__stdcall*)(IDirect3DDevice9*);

Create9Fn g_orig_create9 = nullptr;
CreateDeviceFn g_orig_create_device = nullptr;
EndSceneFn g_orig_end_scene = nullptr;
ResetFn g_orig_reset = nullptr;
BeginStateBlockFn g_orig_begin_state_block = nullptr;

constexpr int kReset = 16, kEndScene = 42, kBeginStateBlock = 60;
// IDirect3DDevice9's slots (IDirect3DDevice9Ex has 134); an adopted device's
// copy has as many as can be read, up to kDeviceCopy.
constexpr int kDeviceSlots = 119, kDeviceCopy = 192;

IDirect3DDevice9* g_device = nullptr;
HWND g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;
bool g_imgui_ready = false;
bool g_visible = false;
int g_toggle_key = VK_F8;

char g_filter[64] = {};
constexpr float kDescColumn = 260.0f;

// Case-insensitive substring test, so the filter box does not drag in shlwapi.
bool contains_ci(const char* haystack, const char* needle) {
  if (!needle || !*needle) return true;
  for (const char* h = haystack; *h; ++h) {
    const char *a = h, *b = needle;
    while (*a && *b && (*a | 0x20) == (*b | 0x20)) ++a, ++b;
    if (!*b) return true;
  }
  return false;
}

// ---------------------------------------------------------------- import hook
// Replace one entry in the target module's import address table.
void* iat_hook(HMODULE module, const char* dll_name, const char* fn_name, void* replacement) {
  auto base = reinterpret_cast<uintptr_t>(module);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

  auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return nullptr;

  for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
       imp->Name; ++imp) {
    const char* name = reinterpret_cast<const char*>(base + imp->Name);
    if (_stricmp(name, dll_name) != 0) continue;

    auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    auto* orig = reinterpret_cast<IMAGE_THUNK_DATA*>(
        base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
    for (; orig->u1.AddressOfData; ++orig, ++thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
      auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), fn_name) != 0) continue;

      void* previous = reinterpret_cast<void*>(thunk->u1.Function);
      if (!mem::write<void*>(reinterpret_cast<uintptr_t>(&thunk->u1.Function), replacement))
        return nullptr;
      return previous;
    }
  }
  return nullptr;
}

// kAwards[] is in database order because that is how the hook identifies an
// award, so the display order is a separate index list.
//
// Which order the game's own Awards screen uses is not recoverable from the
// binary: the rows are handed to Scaleform once, keyed on AchievementIndex, and
// the Flash table sorts them itself. So offer the plausible orderings and let
// the player pick the one that matches.
// Mode 0 reproduces the game's own Awards screen, which sorts alphabetically by
// display name - confirmed against the real screen, where "A-maze-ing" is first.
const char* const kSortModes[] = {"Game order (A-Z)", "Award index", "Points",
                                  "Completed first",  "Database order"};
constexpr int kSortModeCount = static_cast<int>(sizeof(kSortModes) / sizeof(kSortModes[0]));

bool sort_less(int lhs, int rhs, int mode) {
  const AwardDef& a = kAwards[lhs];
  const AwardDef& b = kAwards[rhs];
  auto name_of = [](const AwardDef& x) {
    const char* n = loc::text(x.name_key);
    return n ? n : x.id;
  };
  auto by_name = [&] {
    const int c = _stricmp(name_of(a), name_of(b));
    return c ? c < 0 : a.index < b.index;
  };
  switch (mode) {
    case 1: return a.index < b.index;
    case 2:
      if (a.points != b.points) return a.points > b.points;
      return by_name();
    case 3: {
      const bool ac = awards::effective(lhs), bc = awards::effective(rhs);
      if (ac != bc) return ac;
      return by_name();
    }
    case 4: return lhs < rhs;
    default: return by_name();  // the game's own order
  }
}

const int* display_order() {
  static int order[kAwardCount];
  static int built_for = -1;
  const int mode = awards::settings().sort_mode;
  // Mode 3 and 5 depend on text/state that can change, so rebuild when the mode
  // changes and cheaply re-sort each time those modes are active.
  if (built_for != mode || mode == 0 || mode == 2 || mode == 3) {
    for (int i = 0; i < kAwardCount; ++i) order[i] = i;
    for (int i = 1; i < kAwardCount; ++i) {  // insertion sort; 58 items
      int v = order[i], j = i - 1;
      while (j >= 0 && sort_less(v, order[j], mode)) {
        order[j + 1] = order[j];
        --j;
      }
      order[j + 1] = v;
    }
    built_for = mode;
  }
  return order;
}

// The game's own display names ("Bank Shot", "Deadly Ballet") live in
// Loki.strdb, whose key index we have not fully reversed yet, so present a
// readable form of the record id instead: ACH_MP_BadRobot -> "MP Bad Robot".
// The raw id stays available in the tooltip, and is what the config file uses.
const char* pretty_name(const AwardDef& a) {
  static char buf[96];
  const char* src = a.id;
  if (!std::strncmp(src, "ACH_", 4)) src += 4;

  size_t o = 0;
  for (size_t i = 0; src[i] && o + 2 < sizeof(buf); ++i) {
    char c = src[i];
    if (c == '_') {
      buf[o++] = ' ';
      continue;
    }
    // Split camelCase so "BadRobot" reads as "Bad Robot", but leave runs of
    // capitals like "MP" and "CTF" and digit groups alone.
    if (i > 0 && o > 0 && buf[o - 1] != ' ' && c >= 'A' && c <= 'Z' &&
        ((src[i - 1] >= 'a' && src[i - 1] <= 'z') || (src[i - 1] >= '0' && src[i - 1] <= '9')))
      buf[o++] = ' ';
    buf[o++] = c;
  }
  buf[o] = '\0';
  return buf;
}

// --------------------------------------------------------------------- panel
void draw_panel() {
  ImGui::SetNextWindowSize(ImVec2(900, 560), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("F.E.A.R. 2 - Award Toggles  v" F2AU_VERSION, &g_visible)) {
    ImGui::End();
    return;
  }

  ImGui::TextUnformatted("Toggle any award on or off. Changes apply immediately and are saved.");
  ImGui::Separator();

  if (ImGui::Button("Unlock all")) {
    for (int i = 0; i < kAwardCount; ++i) awards::apply(i, true);
    awards::save_config();
  }
  ImGui::SameLine();
  if (ImGui::Button("Lock all")) {
    for (int i = 0; i < kAwardCount; ++i) awards::apply(i, false);
    awards::save_config();
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear overrides")) {
    for (int i = 0; i < kAwardCount; ++i) awards::set_forced(i, awards::Forced::kDefault);
    awards::save_config();
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(160);
  ImGui::InputTextWithHint("##filter", "filter...", g_filter, sizeof(g_filter));

  ImGui::SameLine();
  ImGui::SetNextItemWidth(150);
  int mode = awards::settings().sort_mode;
  if (mode < 0 || mode >= kSortModeCount) mode = 0;
  if (ImGui::Combo("##sort", &mode, kSortModes, kSortModeCount)) {
    awards::mutable_settings().sort_mode = mode;
    awards::save_config();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Row order. Pick whichever matches the game's own Awards screen.");

  ImGui::Separator();
  int complete = 0;
  for (int i = 0; i < kAwardCount; ++i)
    if (awards::effective(i)) ++complete;
  ImGui::Text("%d of %d awards complete", complete, kAwardCount);

  ImGui::BeginChild("list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),
                    ImGuiChildFlags_Borders);
  const int* order = display_order();
  for (int row = 0; row < kAwardCount; ++row) {
    const int i = order[row];
    const AwardDef& a = kAwards[i];
    if (!contains_ci(a.id, g_filter)) continue;

    // Prefer the game's own localized text; fall back to the record id while the
    // string manager is still coming up.
    const char* name = loc::text(a.name_key);
    const char* desc = loc::text(a.desc_key);
    if (!name) name = pretty_name(a);

    bool on = awards::effective(i);
    ImGui::PushID(i);
    if (ImGui::Checkbox(name, &on)) {
      awards::apply(i, on);
      awards::save_config();
    }
    if (awards::forced(i) != awards::Forced::kDefault) {
      ImGui::SameLine(0, 4);
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "*");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overridden by this mod");
    }

    ImGui::SameLine(kDescColumn);
    if (desc)
      ImGui::TextDisabled("%s", desc);
    else if (!a.counter)
      ImGui::TextDisabled("(Reborn DLC)");
    else
      ImGui::TextDisabled("%s >= %d", a.counter, a.threshold);

    if (ImGui::IsItemHovered()) {
      if (!a.counter)
        ImGui::SetTooltip(
            "%s\n%d points\nsaved as Achievements%s\n\n"
            "Reborn DLC award. The game has no counter that would ever earn it\n"
            "on PC, but it keeps the award itself in the save like any other,\n"
            "so this toggle is permanent.",
            a.id, a.points, a.storage);
      else
        ImGui::SetTooltip("%s\n%d points\nsaved as Achievements%s\nearned at %s >= %d", a.id,
                          a.points, a.storage, a.counter, a.threshold);
    }
    ImGui::PopID();
  }
  ImGui::EndChild();

  const auto& cfg = awards::settings();
  if (!cfg.auto_show)
    ImGui::TextDisabled("Auto-open disabled (AutoShow = 0 in Fear2AwardUnlocker.ini)");
  else if (callbacks::on_awards_screen())
    ImGui::TextDisabled("Opened automatically for the Awards screen");
  else
    ImGui::TextDisabled("Opened with the hotkey");

  ImGui::End();
}

// Log every award's localized name once, so the on-screen order can be compared
// against the orderings the mod can produce.
void dump_names_once() {
  static bool done = false;
  if (done) return;
  const char* probe = loc::text(kAwards[0].name_key);
  if (!probe) return;  // string manager not up yet; try again next frame
  done = true;

  logf("--- localized award names (database order) ---");
  for (int i = 0; i < kAwardCount; ++i) {
    const char* n = loc::text(kAwards[i].name_key);
    logf("  [%2d] idx=%-3d order=%-3d %-28s %s", i, kAwards[i].index, kAwards[i].order,
         kAwards[i].id, n ? n : "(no name)");
  }
  logf("--- same names, sorted A-Z ---");
  int order[kAwardCount];
  for (int i = 0; i < kAwardCount; ++i) order[i] = i;
  for (int i = 1; i < kAwardCount; ++i) {
    int v = order[i], j = i - 1;
    auto key = [](int k) {
      const char* n = loc::text(kAwards[k].name_key);
      return n ? n : kAwards[k].id;
    };
    while (j >= 0 && _stricmp(key(v), key(order[j])) < 0) {
      order[j + 1] = order[j];
      --j;
    }
    order[j + 1] = v;
  }
  for (int r = 0; r < kAwardCount; ++r) {
    const int i = order[r];
    const char* n = loc::text(kAwards[i].name_key);
    logf("  %2d. %-30s (%s)", r + 1, n ? n : kAwards[i].id, kAwards[i].id);
  }
}

// ------------------------------------------------------------- screen tracking
// Opening the Awards screen runs no game code at all, so there is nothing to
// hook directly; callbacks.cpp infers it from the shape of the menu bridge
// traffic instead. This just follows that inference.
bool g_auto_opened = false;

void track_menu_state() {
  if (!awards::settings().auto_show) return;

  const bool on_awards = callbacks::on_awards_screen();
  if (on_awards && !g_visible) {
    g_visible = true;
    g_auto_opened = true;
    logf("Awards screen detected - opening the panel");
  } else if (!on_awards && g_visible && g_auto_opened) {
    g_visible = false;
    g_auto_opened = false;
    logf("left the Awards screen - closing the panel");
  }
}

// -------------------------------------------------------------------- hooks
LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_KEYDOWN && static_cast<int>(wp) == g_toggle_key) {
    g_visible = !g_visible;
    g_auto_opened = false;  // a manual toggle takes precedence over the automatic one
    logf("overlay %s", g_visible ? "shown" : "hidden");
    return 0;
  }
  // Escape backs out of the Awards screen, so let it dismiss the panel too -
  // but pass it through, so the game still navigates back.
  if (msg == WM_KEYDOWN && wp == VK_ESCAPE && g_visible) g_visible = false;
  // Only swallow input while the panel is up, so the game plays normally
  // otherwise.
  if (g_visible && g_imgui_ready) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 1;
    if (io.WantCaptureKeyboard && msg >= WM_KEYFIRST && msg <= WM_KEYLAST) return 1;
  }
  return CallWindowProcA(g_orig_wndproc, hwnd, msg, wp, lp);
}

void init_imgui(IDirect3DDevice9* device) {
  if (g_imgui_ready) return;

  if (!g_hwnd) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (SUCCEEDED(device->GetCreationParameters(&cp))) g_hwnd = cp.hFocusWindow;
  }
  if (!g_hwnd) return;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // do not scatter imgui.ini into the game folder
  ImGui::StyleColorsDark();

  if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX9_Init(device)) {
    logf("ERROR: ImGui backend init failed");
    return;
  }
  g_orig_wndproc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hk_wndproc)));

  g_toggle_key = awards::settings().toggle_key;
  g_visible = awards::settings().show_on_start;
  g_imgui_ready = true;
  logf("overlay ready (hwnd=%p) - press virtual key 0x%02X in game to open the award panel",
       g_hwnd, g_toggle_key);
}

// ------------------------------------------------ a vtable the device keeps
// Windows' d3d9.dll gives each device a vtable of its own, inside the device
// (32-bit d3d9.dll of Windows 11 build 26200: as it makes the device it copies
// the class's 175 slots to +0x2F5C and points the device there). Two things
// rely on the device staying on that table:
//  - d3d9 keeps rewriting it. BeginStateBlock and a Reset that fails copy the
//    class's first 119 slots back over it, and fast paths go into 17 of the
//    state-setting slots. D3DX records state blocks through the device's
//    vtable (d3dx9_40.dll: BeginStateBlock, SetViewport, EndStateBlock): on a
//    copy the device would go on calling the fast paths while d3d9 records.
//  - The Windows shim engine (apphelp.dll; FEAR2.exe got DWM8And16BitMitigation
//    on its first launch on the laptop) hooks a COM object in place in the vtable it has -
//    QueryInterface and Release on every object, CreateAdditionalSwapChain and
//    Reset on a device - and each hook looks up what to call on by the vtable
//    the object points to at the call. Found nothing, QueryInterface fails
//    (E_NOINTERFACE), Release does nothing, and Reset and
//    CreateAdditionalSwapChain call address 0.
// 1.0.1 moved the game's device to a copy of the mod's (adopt.cpp), and the
// game crashed at its first start on Windows, 2 s after: a null D3DX effect in
// its renderer (2026-10-22). So a device whose vtable is in no module - its own
// - is hooked where it is. That table is the device's alone, so no other device
// shows the mod's functions to the Steam overlay, which is what the copy was
// for. A class's vtable that something has hooked QueryInterface or Release in
// place (the shim engine's mark) is hooked where it is as well, since a copy
// would break that hooker for certain; there the mod's functions come out of
// it while the next device is made (hk_create_device), so the overlay's pass
// over that device does not find them either.
//
// After a call that can rewrite the table, a slot that changed during it gets
// the mod's function back, calling on to what is there now.
HRESULT __stdcall hk_end_scene(IDirect3DDevice9* device);
HRESULT __stdcall hk_reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp);
HRESULT __stdcall hk_begin_state_block(IDirect3DDevice9* device);

void** g_kept_vt = nullptr;  // the game's device's vtable, hooked where it is (Windows)
bool g_kept_on_heap = false;  // its own: plain memory, rewritten on every BeginStateBlock
constexpr int kKeptSlots[] = {kEndScene, kReset, kBeginStateBlock};

void kept_write(void** entry, void* fn) {
  if (g_kept_on_heap)
    InterlockedExchangePointer(entry, fn);
  else
    mem::write<void*>(reinterpret_cast<uintptr_t>(entry), fn);
}

void* kept_hook_fn(int slot) {
  switch (slot) {
    case kEndScene: return reinterpret_cast<void*>(&hk_end_scene);
    case kReset: return reinterpret_cast<void*>(&hk_reset);
    default: return reinterpret_cast<void*>(&hk_begin_state_block);
  }
}

void* kept_orig(int slot) {
  switch (slot) {
    case kEndScene: return reinterpret_cast<void*>(g_orig_end_scene);
    case kReset: return reinterpret_cast<void*>(g_orig_reset);
    default: return reinterpret_cast<void*>(g_orig_begin_state_block);
  }
}

// The mod's function into `slot`; what the slot held is what it calls on to.
void kept_hook(int slot) {
  void* const fn = kept_hook_fn(slot);
  void* const was = g_kept_vt[slot];
  if (was == fn) return;
  switch (slot) {
    case kEndScene: g_orig_end_scene = reinterpret_cast<EndSceneFn>(was); break;
    case kReset: g_orig_reset = reinterpret_cast<ResetFn>(was); break;
    default: g_orig_begin_state_block = reinterpret_cast<BeginStateBlockFn>(was); break;
  }
  kept_write(&g_kept_vt[slot], fn);
}

// What each slot held goes back where the mod's function still is.
void kept_unhook(void** vt) {
  for (int slot : kKeptSlots)
    if (void* orig = kept_orig(slot); orig && vt[slot] == kept_hook_fn(slot)) kept_write(&vt[slot], orig);
}

// The kept slots before a call that can rewrite the table (none for a device
// that is not the one hooked where it is).
struct KeptSlots {
  bool kept = false;
  void* fn[3] = {};
};

KeptSlots kept_before(IDirect3DDevice9* device) {
  KeptSlots s;
  s.kept = g_kept_vt && mem::read<void**>(reinterpret_cast<uintptr_t>(device)) == g_kept_vt;
  if (s.kept)
    for (int i = 0; i < 3; ++i) s.fn[i] = g_kept_vt[kKeptSlots[i]];
  return s;
}

void kept_after(const KeptSlots& before, const char* call) {
  if (!before.kept || !g_kept_vt) return;
  int rewritten = 0;
  for (int i = 0; i < 3; ++i)
    if (g_kept_vt[kKeptSlots[i]] != before.fn[i]) {
      kept_hook(kKeptSlots[i]);
      ++rewritten;
    }
  static volatile LONG logged = 0;
  if (!rewritten) return;
  const LONG n = InterlockedIncrement(&logged);
  if (n <= 3)
    logf("%s rewrote the device's vtable - its EndScene/Reset are the mod's again%s", call,
         n == 3 ? " (no more of these logged)" : "");
}

// Why the game's device keeps its vtable, or null for a copy (adopt.cpp).
const char* keeps_vtable(void** vt) {
  const HMODULE owner = mem::module_of(vt);
  if (!owner) return "its own";
  if (mem::module_of(vt[0]) != owner || mem::module_of(vt[2]) != owner)
    return "its class's, QueryInterface/Release hooked in place by another module";
  return nullptr;
}

// "; the Windows shim engine (apphelp.dll) has slots 0 2 13 16", for the log.
void shim_slots(void* const* vt, char* out, size_t n) {
  out[0] = '\0';
  const mem::Range shim = mem::module_range(GetModuleHandleA("apphelp.dll"));
  if (shim.empty()) return;
  size_t o = 0;
  for (int s = 0; s < kDeviceSlots; ++s) {
    if (!shim.contains(reinterpret_cast<uintptr_t>(vt[s]))) continue;
    const int w = std::snprintf(out + o, n - o, "%s %d", o ? "" : "; the Windows shim engine (apphelp.dll) has slots", s);
    if (w < 0 || static_cast<size_t>(w) >= n - o) break;
    o += static_cast<size_t>(w);
  }
}

HRESULT __stdcall hk_end_scene(IDirect3DDevice9* device) {
  const auto orig = reinterpret_cast<EndSceneFn>(
      adopt::original(device, kEndScene, reinterpret_cast<void*>(g_orig_end_scene)));
  // An EndScene that comes back in while ours is on its way on (another tool's
  // hook calling the device's vtable again) is the same frame: straight through.
  if (adopt::calling_on()) return orig(device);
  init_imgui(device);

  hooks::poll();
  callbacks::poll();
  dump_names_once();
  track_menu_state();

  if (g_imgui_ready && g_visible) {
    // The game leaves render state in whatever the last draw used; ImGui's DX9
    // backend sets up everything it needs, but it does not restore ours, so save
    // and restore the block around the overlay draw.
    IDirect3DStateBlock9* state = nullptr;
    device->CreateStateBlock(D3DSBT_ALL, &state);

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_panel();
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    if (state) {
      state->Apply();
      state->Release();
    }
  }
  adopt::CallingOn on;
  return orig(device);
}

HRESULT __stdcall hk_reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
  if (g_imgui_ready) ImGui_ImplDX9_InvalidateDeviceObjects();
  const auto orig = reinterpret_cast<ResetFn>(adopt::original(device, kReset, reinterpret_cast<void*>(g_orig_reset)));
  const KeptSlots before = kept_before(device);
  HRESULT hr;
  {
    adopt::CallingOn on;
    hr = orig(device, pp);
  }
  kept_after(before, "Reset");
  if (g_imgui_ready) ImGui_ImplDX9_CreateDeviceObjects();
  return hr;
}

// Only in a vtable the device keeps, where d3d9 rewrites the table (above).
HRESULT __stdcall hk_begin_state_block(IDirect3DDevice9* device) {
  const KeptSlots before = kept_before(device);
  const HRESULT hr = g_orig_begin_state_block(device);
  kept_after(before, "BeginStateBlock");
  return hr;
}

HRESULT __stdcall hk_create_device(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                   DWORD flags, D3DPRESENT_PARAMETERS* pp,
                                   IDirect3DDevice9** out) {
  // A class's vtable hooked where it is would show the mod's functions to the
  // Steam overlay's pass over the new device: out of it for the call.
  void** const shared = g_kept_vt && !g_kept_on_heap ? g_kept_vt : nullptr;
  if (shared) kept_unhook(shared);
  HRESULT hr = g_orig_create_device(self, adapter, type, focus, flags, pp, out);
  if (shared)
    for (int slot : kKeptSlots) kept_hook(slot);
  if (FAILED(hr) || !out || !*out) return hr;

  IDirect3DDevice9* device = *out;
  HWND hwnd = focus ? focus : pp ? pp->hDeviceWindow : nullptr;
  auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(device));
  if (adopt::enabled()) {
    if (!adopt::game_window(hwnd)) {
      adopt::leave_alone("a D3D9 device", "CreateDevice", hwnd);
      return hr;
    }
    char where[MAX_PATH + 32], win[128];
    describe_address(reinterpret_cast<void*>(vt), where, sizeof(where));
    adopt::describe_window(hwnd, win, sizeof(win));
    void** const table = reinterpret_cast<void**>(vt);
    if (const char* whose = keeps_vtable(table)) {
      char shim[160];
      shim_slots(table, shim, sizeof(shim));
      const uintptr_t inside = vt - reinterpret_cast<uintptr_t>(device);
      if (inside < 0x10000) std::snprintf(where, sizeof(where), "+0x%X in the device", static_cast<unsigned>(inside));
      g_kept_vt = table;
      g_kept_on_heap = !mem::module_of(table);
      for (int slot : kKeptSlots) kept_hook(slot);
      logf("D3D9 device %p (%s) keeps its vtable (%s, at %s) - EndScene/Reset hooked in it%s", device, win, whose,
           where, shim);
    } else {
      static const adopt::Hook hooks[] = {{kEndScene, reinterpret_cast<void*>(&hk_end_scene)},
                                          {kReset, reinterpret_cast<void*>(&hk_reset)}};
      // What the hooks fall back on: the class's functions (its vtable is left alone).
      if (!g_orig_end_scene) {
        g_orig_end_scene = mem::read<EndSceneFn>(vt + kEndScene * sizeof(void*));
        g_orig_reset = mem::read<ResetFn>(vt + kReset * sizeof(void*));
      }
      const int copied = adopt::take(device, kDeviceSlots, kDeviceCopy, hooks, 2);
      if (!copied) {
        logf("ERROR: the game's D3D9 device %p (%s) could not be adopted (its vtable at %s) - no panel", device, win,
             where);
        return hr;
      }
      logf("D3D9 device %p (%s) adopted - a vtable of its own with EndScene/Reset (%d slots copied; the class's at %s)",
           device, win, copied, where);
    }
  } else if (!g_orig_end_scene) {
    g_orig_end_scene = reinterpret_cast<EndSceneFn>(
        mem::hook_vtable(vt, kEndScene, reinterpret_cast<void*>(&hk_end_scene)));
    g_orig_reset =
        reinterpret_cast<ResetFn>(mem::hook_vtable(vt, kReset, reinterpret_cast<void*>(&hk_reset)));
    logf("captured D3D9 device %p (hwnd=%p); hooked EndScene/Reset", device, hwnd);
  }
  g_device = device;
  if (hwnd) g_hwnd = hwnd;
  return hr;
}

IDirect3D9* WINAPI hk_create9(UINT sdk) {
  IDirect3D9* d3d = g_orig_create9(sdk);
  if (d3d && !g_orig_create_device) {
    auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(d3d));
    g_orig_create_device = reinterpret_cast<CreateDeviceFn>(
        mem::hook_vtable(vt, 16, reinterpret_cast<void*>(&hk_create_device)));
    logf("Direct3DCreate9 intercepted; hooked IDirect3D9::CreateDevice");
  }
  return d3d;
}

}  // namespace

// uninstall() is defined below.
void uninstall();

void on_game_exiting() {
  static bool done = false;
  if (done) return;
  done = true;
  logf("game is exiting - releasing overlay resources early");

  g_visible = false;
  hooks::uninstall();  // take every game patch out while the game is still healthy
  if (g_imgui_ready) {
    // Drop the vertex/index buffers and font texture we are holding on the
    // device, then stop the backends touching it again.
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    g_imgui_ready = false;
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
  }
  uninstall();
}

void uninstall() {
  // Put the window procedure back first: a message arriving after our image is
  // gone would jump into freed memory.
  if (g_orig_wndproc && g_hwnd && IsWindow(g_hwnd)) {
    SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
    g_orig_wndproc = nullptr;
  }

  // The adopted device's copy gets the class's functions back (the copy is the
  // mod's own, so this holds whether or not the device is still alive); a
  // vtable the device kept gets back what its slots held, once, while the game
  // is still healthy (the device's own goes with the device); otherwise restore
  // the class's vtable if the device is still alive.
  if (adopt::enabled()) {
    adopt::unhook_all();
    if (void** vt = g_kept_vt) {
      g_kept_vt = nullptr;
      if (mem::readable(vt, (kBeginStateBlock + 1) * sizeof(void*))) kept_unhook(vt);
    }
  } else if (g_device && !IsBadReadPtr(g_device, sizeof(void*))) {
    auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(g_device));
    if (vt && !IsBadReadPtr(reinterpret_cast<void*>(vt), 64 * sizeof(void*))) {
      if (g_orig_end_scene)
        mem::write<void*>(vt + 42 * sizeof(void*), reinterpret_cast<void*>(g_orig_end_scene));
      if (g_orig_reset)
        mem::write<void*>(vt + 16 * sizeof(void*), reinterpret_cast<void*>(g_orig_reset));
    }
  }
  g_orig_end_scene = nullptr;
  g_orig_reset = nullptr;
  g_imgui_ready = false;  // stop drawing; the context is left for the OS to reclaim
  logf("overlay hooks removed");
}

bool install() {
  HMODULE exe = GetModuleHandleA(nullptr);
  g_orig_create9 = reinterpret_cast<Create9Fn>(
      iat_hook(exe, "d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&hk_create9)));
  if (!g_orig_create9) {
    logf("ERROR: could not hook Direct3DCreate9 in the import table - no overlay");
    return false;
  }
  logf("overlay: Direct3DCreate9 import hooked (original %p)",
       reinterpret_cast<void*>(g_orig_create9));
  return true;
}

}  // namespace f2au::overlay
