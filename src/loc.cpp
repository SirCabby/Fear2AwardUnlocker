#include "loc.h"

#include <windows.h>

#include <cstring>
#include <map>
#include <string>

#include "gameclient.h"
#include "log.h"
#include "mem.h"

namespace f2au::loc {
namespace {

// __thiscall on the string manager: (ctx, key) -> const wchar_t*.
// __fastcall with a dummy edx parameter matches that shape under GCC.
using LocalizeFn = const wchar_t*(__fastcall*)(void* self, void* edx, void* ctx, const char* key);

std::map<std::string, std::string> g_cache;
bool g_warned = false;

}  // namespace

const char* text(const char* key) {
  if (!key || !*key) return nullptr;

  auto it = g_cache.find(key);
  if (it != g_cache.end()) return it->second.empty() ? nullptr : it->second.c_str();

  HMODULE gc = GetModuleHandleA("gameclient.dll");
  if (!gc) return nullptr;
  auto base = reinterpret_cast<uintptr_t>(gc);

  void* mgr = mem::read<void*>(base + gc::kStringMgr);
  void* ctx = mem::read<void*>(base + gc::kStringCtx);
  if (!mgr || !ctx) {
    if (!g_warned) {
      logf("localization: string manager not ready yet (mgr=%p ctx=%p)", mgr, ctx);
      g_warned = true;
    }
    return nullptr;  // not cached, so we retry on a later frame
  }

  auto vtable = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(mgr));
  auto fn = reinterpret_cast<LocalizeFn>(
      mem::read<void*>(vtable + gc::kLocalizeVtableSlot * sizeof(void*)));
  if (!fn) return nullptr;

  const wchar_t* wide = fn(mgr, nullptr, ctx, key);

  std::string utf8;
  // The game substitutes "" or "[" when a key is missing; treat both as absent.
  if (wide && wide[0] && !(wide[0] == L'[' && wide[1] == L'\0')) {
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
      utf8.resize(static_cast<size_t>(n) - 1);
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), n, nullptr, nullptr);
    }
  }

  auto [pos, _] = g_cache.emplace(key, std::move(utf8));
  return pos->second.empty() ? nullptr : pos->second.c_str();
}

}  // namespace f2au::loc
