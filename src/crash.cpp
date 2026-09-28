#include "crash.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "log.h"
#include "mem.h"

namespace f2au {
namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

const char* code_name(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    default: return "exception";
  }
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
  if (info && info->ExceptionRecord) {
    void* at = info->ExceptionRecord->ExceptionAddress;

    // Name the module the faulting address belongs to - that is the one fact
    // that decides whether this mod is to blame.
    char module[MAX_PATH] = "<unknown>";
    HMODULE owner = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(at), &owner) &&
        owner) {
      GetModuleFileNameA(owner, module, MAX_PATH);
    }
    logf("CRASH: %s (0x%08lX) at %p in %s", code_name(info->ExceptionRecord->ExceptionCode),
         info->ExceptionRecord->ExceptionCode, at, module);
    if (info->ContextRecord)
      logf("       eip=%08lX esp=%08lX ebp=%08lX eax=%08lX ecx=%08lX edx=%08lX",
           info->ContextRecord->Eip, info->ContextRecord->Esp, info->ContextRecord->Ebp,
           info->ContextRecord->Eax, info->ContextRecord->Ecx, info->ContextRecord->Edx);
  }
  return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// R6025 "pure virtual function call" does not raise an SEH exception - the CRT
// calls _purecall, which prints the dialog and aborts - so the filter above
// never sees it. Every game module imports _purecall from MSVCR80, so hook that
// import instead and record who called it.
using PurecallFn = void(__cdecl*)();
PurecallFn g_orig_purecall = nullptr;

void describe(const void* addr, char* out, size_t n) { describe_address(addr, out, n); }

void __cdecl hk_purecall() {
  void* caller = __builtin_return_address(0);
  char where[MAX_PATH + 32];
  describe(caller, where, sizeof(where));
  logf("PURECALL (R6025): pure virtual called from %s", where);

  // Walk a little of the stack for extra context - return addresses that land
  // inside a loaded module are the useful ones.
  auto* frame = reinterpret_cast<void**>(__builtin_frame_address(0));
  for (int i = 0, shown = 0; i < 64 && shown < 6; ++i) {
    void* v = frame[i];
    if (!v) continue;
    HMODULE owner = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(v), &owner) &&
        owner) {
      describe(v, where, sizeof(where));
      logf("    stack[%02d] %s", i, where);
      ++shown;
    }
  }
  if (g_orig_purecall) g_orig_purecall();
}

}  // namespace

void describe_address(const void* addr, char* out, size_t n) {
  HMODULE owner = nullptr;
  char path[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(addr), &owner) &&
      owner) {
    GetModuleFileNameA(owner, path, MAX_PATH);
    const char* leaf = std::strrchr(path, '\\');
    std::snprintf(out, n, "%s+0x%X", leaf ? leaf + 1 : path,
                  static_cast<unsigned>(reinterpret_cast<uintptr_t>(addr) -
                                        reinterpret_cast<uintptr_t>(owner)));
  } else {
    std::snprintf(out, n, "%p (no module)", addr);
  }
}

void install_purecall_logger() {
  const char* modules[] = {nullptr, "gameclient.dll", "gameserver.dll"};
  for (const char* m : modules) {
    HMODULE h = m ? GetModuleHandleA(m) : GetModuleHandleA(nullptr);
    if (!h) continue;
    void* prev = mem::iat_hook(h, "MSVCR80.dll", "_purecall",
                               reinterpret_cast<void*>(&hk_purecall));
    if (prev) {
      if (!g_orig_purecall) g_orig_purecall = reinterpret_cast<PurecallFn>(prev);
      logf("hooked _purecall in %s", m ? m : "FEAR2.exe");
    }
  }
}

void install_crash_logger() { g_previous = SetUnhandledExceptionFilter(on_exception); }

}  // namespace f2au
