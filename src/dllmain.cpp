// Fear2AwardUnlocker - proxy entry point.
//
// We ship as LTMemory.dll and sit in front of the game's own LTMemory.dll,
// which `make install` renames to LTMemory_orig.dll. FEAR2.exe statically
// imports exactly one symbol from it:
//
//     ?GetILTMemory@@YAPAVILTMemory@@XZ   ==   ILTMemory* __cdecl GetILTMemory()
//
// so all we owe the loader is that one function. ltmemory.def aliases the
// MSVC-mangled export name onto proxy_GetILTMemory below.
//
// Why LTMemory and not d3d9/dinput8: those are Wine builtins, so proxying them
// needs a WINEDLLOVERRIDES launch option under Proton. ltmemory is not, so our
// file simply wins the application-directory search on Windows and Wine alike -
// the mod is a pure drop-in on both.

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "adopt.h"
#include "awards.h"
#include "hooks.h"
#include "crash.h"
#include "log.h"
#include "overlay.h"
#include "version.h"

namespace {

constexpr const char* kOriginalDll = "LTMemory_orig.dll";
constexpr const char* kExportName  = "?GetILTMemory@@YAPAVILTMemory@@XZ";

using GetILTMemoryFn = void*(__cdecl*)();

HMODULE        g_self               = nullptr;
HMODULE        g_original           = nullptr;
GetILTMemoryFn g_original_fn        = nullptr;
char           g_dir[MAX_PATH]      = {};
bool           g_forward_resolved   = false;

// Directory this DLL was loaded from, with a trailing separator. We load the
// renamed original by absolute path rather than trusting the search order.
void resolve_own_dir() {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(g_self, path, MAX_PATH);
  char* slash = strrchr(path, '\\');
  if (!slash) slash = strrchr(path, '/');
  if (slash) {
    size_t len = static_cast<size_t>(slash - path) + 1;
    if (len < sizeof(g_dir)) {
      memcpy(g_dir, path, len);
      g_dir[len] = '\0';
    }
  }
}

// Loaded from DLL_PROCESS_ATTACH, deliberately, even though LoadLibrary under
// the loader lock is normally something to avoid.
//
// LTMemory.dll is the engine's allocator, and GetILTMemory() hands back a C++
// interface whose vtable lives inside it. Loading it lazily (on the first call)
// makes it the most recently initialised module, so the loader unloads it FIRST
// on the way out - while the game still has destructors that free memory
// through that interface. Calling a virtual on the by-then destructed allocator
// is a pure virtual call: the R6025 dialog on exit.
//
// Loading it here instead puts it back where the stock game has it: initialised
// before the exe's entry point, and torn down late. It only imports KERNEL32
// and MSVCR80, so it is safe to bring in under the lock.
void resolve_forward() {
  if (g_forward_resolved) return;
  g_forward_resolved = true;

  char path[MAX_PATH] = {};
  snprintf(path, sizeof(path), "%s%s", g_dir, kOriginalDll);

  g_original = LoadLibraryA(path);
  // Pin it as well, so nothing can drop the last reference early.
  if (g_original) {
    HMODULE pinned = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, path, &pinned);
  }
  if (!g_original) {
    f2au::logf("FATAL: could not load %s (GetLastError=%lu). Did `make install` run?",
               path, GetLastError());
    return;
  }

  g_original_fn =
      reinterpret_cast<GetILTMemoryFn>(
          reinterpret_cast<void*>(GetProcAddress(g_original, kExportName)));
  if (!g_original_fn) {
    f2au::logf("FATAL: %s has no export %s (GetLastError=%lu)", kOriginalDll, kExportName,
               GetLastError());
    return;
  }

  f2au::logf("forwarding %s -> %s", kExportName, path);
}

// Mod init runs off the loader lock: gameclient.dll is not mapped yet when our
// DllMain runs, and hooking has to wait for it.
DWORD WINAPI mod_thread(LPVOID) {
  f2au::logf("mod thread started");
  f2au::awards::load_config(g_dir);
  if (!f2au::awards::settings().disable_overlay) f2au::overlay::install();
  f2au::hooks::install();
  return 0;
}

}  // namespace

// Exported under the game's MSVC-mangled name via ltmemory.def, not dllexport.
extern "C" void* __cdecl proxy_GetILTMemory() {
  resolve_forward();
  if (!g_original_fn) return nullptr;
  return g_original_fn();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH: {
      g_self = module;
      DisableThreadLibraryCalls(module);
      resolve_own_dir();
      f2au::log_init(g_dir);
      f2au::logf("Fear2AwardUnlocker " F2AU_VERSION " loaded (LTMemory.dll proxy), dir=%s", g_dir);

      // Bring the real allocator in now, not on first use - see resolve_forward().
      resolve_forward();

      f2au::install_crash_logger();
      // Here, not on the mod thread: this is the game's primary thread, the one
      // that makes its window (adopt::game_window).
      f2au::adopt::init();
      if (HANDLE t = CreateThread(nullptr, 0, mod_thread, nullptr, 0, nullptr)) CloseHandle(t);
      break;
    }
    case DLL_PROCESS_DETACH:
      // Undo every patch before this image goes away. Anything still pointing
      // into the DLL - the window procedure, the D3D9 vtable, the achievement
      // vtable, the 162 menu-callback thunks - would fault the moment the game
      // touched it during its own teardown.
      f2au::logf("unloading - removing hooks");
      f2au::overlay::uninstall();
      f2au::hooks::uninstall();
      f2au::logf("hooks removed cleanly");
      f2au::log_shutdown();
      break;
    default:
      break;
  }
  return TRUE;
}
