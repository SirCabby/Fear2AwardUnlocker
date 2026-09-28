#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>

// Small memory helpers, in the spirit of Zeal's memory.{h,cpp}: everything the
// mod pokes at lives in another module's read-only pages, so every write has to
// unprotect, write, restore and flush.
namespace f2au::mem {

struct Range {
  uintptr_t begin = 0;
  uintptr_t end = 0;
  bool empty() const { return end <= begin; }
  bool contains(uintptr_t a) const { return a >= begin && a < end; }
};

template <typename T>
inline bool write(uintptr_t target, const T& value) {
  DWORD old = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(target), sizeof(T), PAGE_EXECUTE_READWRITE, &old))
    return false;
  std::memcpy(reinterpret_cast<void*>(target), &value, sizeof(T));
  VirtualProtect(reinterpret_cast<void*>(target), sizeof(T), old, &old);
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), sizeof(T));
  return true;
}

template <typename T>
inline T read(uintptr_t target) {
  T v{};
  std::memcpy(&v, reinterpret_cast<const void*>(target), sizeof(T));
  return v;
}

// True when [p, p+n) is committed, readable memory. VirtualQuery-based, so it
// is safe to call on any value that merely looks like a pointer.
inline bool readable(const void* p, size_t n) {
  auto a = reinterpret_cast<uintptr_t>(p);
  if (!a) return false;
  while (true) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return false;
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const size_t avail = region_end - a;
    if (avail >= n) return true;
    n -= avail;
    a = region_end;
  }
}

template <typename T>
inline bool read_safe(uintptr_t target, T* out) {
  if (!readable(reinterpret_cast<const void*>(target), sizeof(T))) return false;
  std::memcpy(out, reinterpret_cast<const void*>(target), sizeof(T));
  return true;
}

// The address range a loaded module's image covers.
inline Range module_range(HMODULE m) {
  auto base = reinterpret_cast<uintptr_t>(m);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (!m || dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return {};
  return Range{base, base + nt->OptionalHeader.SizeOfImage};
}

// Replace one entry of a vtable and hand back what was there. gameclient.dll is
// relocated at load time (it does not get its preferred 0x10000000 base), so
// every address must be formed as module_base + RVA - never used absolute.
inline void* hook_vtable(uintptr_t vtable, int slot, void* replacement) {
  uintptr_t entry = vtable + static_cast<uintptr_t>(slot) * sizeof(void*);
  void* original = read<void*>(entry);
  if (!write<void*>(entry, replacement)) return nullptr;
  return original;
}

// Replace one entry in a module's import address table; returns what was there.
inline void* iat_hook(HMODULE module, const char* dll_name, const char* fn_name,
                      void* replacement) {
  auto base = reinterpret_cast<uintptr_t>(module);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
  auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return nullptr;

  for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
       imp->Name; ++imp) {
    const char* name = reinterpret_cast<const char*>(base + imp->Name);
    if (dll_name && _stricmp(name, dll_name) != 0) continue;
    auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    auto* orig = reinterpret_cast<IMAGE_THUNK_DATA*>(
        base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
    for (; orig->u1.AddressOfData; ++orig, ++thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
      auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), fn_name) != 0) continue;
      void* previous = reinterpret_cast<void*>(thunk->u1.Function);
      if (!write<void*>(reinterpret_cast<uintptr_t>(&thunk->u1.Function), replacement))
        return nullptr;
      return previous;
    }
  }
  return nullptr;
}

// Redirect a relative CALL (E8 rel32) at `site` to `replacement`, returning the
// address it used to reach. Used where a function has exactly one call site and
// a full detour would be overkill.
inline void* hook_call_site(uintptr_t site, void* replacement) {
  if (read<uint8_t>(site) != 0xE8) return nullptr;
  const int32_t old_rel = read<int32_t>(site + 1);
  void* previous = reinterpret_cast<void*>(site + 5 + old_rel);
  const int32_t new_rel =
      static_cast<int32_t>(reinterpret_cast<uintptr_t>(replacement) - (site + 5));
  if (!write<int32_t>(site + 1, new_rel)) return nullptr;
  return previous;
}

}  // namespace f2au::mem
