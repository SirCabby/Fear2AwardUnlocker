// The overlay's D3D9 vtable handling with the Steam overlay's way of hooking, outside the game:
//
//   mkdir -p tests/build
//   i686-w64-mingw32-g++ -std=c++20 -O2 -Wall -DWIN32_LEAN_AND_MEAN -static tests/test_adopt.cpp
//       -ld3d9 -luser32 -lgdi32 -o tests/build/test_adopt.exe
//   cp build/LTMemory.dll "$GAME_DIR/LTMemory_orig.dll" <dir>/      (a scratch folder)
//   F2AU_ADOPT=1 wine tests/build/test_adopt.exe 'Z:\<dir>\' overlay adopt
//   wine tests/build/test_adopt.exe 'Z:\<dir>\' none class      (Wine's own way, as before)
//
// (one process per run: the proxy keeps its state; F2AU_ADOPT makes it take
// the Windows path under Wine). The exe imports Direct3DCreate9 as FEAR2.exe
// does, loads the proxy on its primary thread and waits for the proxy's mod
// thread to patch that import, then makes its devices through it.
//
// "overlay" plays the Steam overlay on Windows (gameoverlayrenderer.dll,
// 2026-09-27): installed before the proxy, its CreateDevice hook hooks every
// device as it is made, by what the device class's vtable points to at that
// moment - here by writing its own function into that slot, which a caller of
// the slot meets the same way as the overlay's jump in the function - keeping
// ONE saved original per hook and skipping a slot it hooked already. With the
// proxy's Reset in the class's vtable (1.0.0) the pass for a second device
// takes it for the original and the two call each other at the next Reset: a
// stack overflow on Windows. Here the overlay's hooks count their depth and
// give up past 4 (a "LOOP" failure). With the fix each device for the test's
// window (made by its primary thread) must come back with a vtable of its own
// holding the proxy's hooks, the class's vtable must hold only the overlay's,
// every call must pass through the overlay exactly once, and a device for
// another thread's window must keep its class's vtable.
#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;
HMODULE g_proxy = nullptr;
bool g_expect_adopt = false;

void check(bool ok, const char* what) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

bool in_proxy(const void* fn) {
  HMODULE owner = nullptr;
  return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(fn), &owner) &&
         owner == g_proxy;
}

void** vtable_of(const void* obj) { return *static_cast<void** const*>(obj); }

void write_slot(void** vt, int slot, void* fn) {
  DWORD old = 0;
  VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
  vt[slot] = fn;
  VirtualProtect(&vt[slot], sizeof(void*), old, &old);
}

// This exe's import slot for d3d9.dll!Direct3DCreate9.
void* volatile* import_slot(const char* dll, const char* name) {
  auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
    auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    auto* orig = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
    for (; orig->u1.AddressOfData; ++orig, ++thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
      auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
      if (!std::strcmp(reinterpret_cast<const char*>(by_name->Name), name))
        return reinterpret_cast<void* volatile*>(&thunk->u1.Function);
    }
  }
  return nullptr;
}

// --- the overlay -------------------------------------------------------------
struct OvHook {
  void* saved = nullptr;  // ONE original, whatever the slot held at the last pass
  int calls = 0;
  int depth = 0;
  bool looped = false;
};
OvHook g_reset, g_present9;
void* g_real_create_device = nullptr;
bool g_overlay = false;

template <typename Fn, typename... A>
HRESULT through(OvHook& h, A... a) {
  ++h.calls;
  if (h.depth >= 4) {
    h.looped = true;
    return E_FAIL;
  }
  ++h.depth;
  const HRESULT hr = reinterpret_cast<Fn>(h.saved)(a...);
  --h.depth;
  return hr;
}

using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Present9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                   IDirect3DDevice9**);

HRESULT STDMETHODCALLTYPE ov_reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) { return through<ResetFn>(g_reset, d, pp); }
HRESULT STDMETHODCALLTYPE ov_present9(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
  return through<Present9Fn>(g_present9, d, a, b, w, r);
}

// Each slot, unless it is the overlay's already.
void ov_hook(void** vt, int slot, void* hook, OvHook& h) {
  if (vt[slot] == hook) return;
  h.saved = vt[slot];
  write_slot(vt, slot, hook);
}

HRESULT STDMETHODCALLTYPE ov_create_device(IDirect3D9* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD fl, D3DPRESENT_PARAMETERS* pp,
                                           IDirect3DDevice9** out) {
  const HRESULT hr = reinterpret_cast<CreateDeviceFn>(g_real_create_device)(d3d, a, t, w, fl, pp, out);
  if (SUCCEEDED(hr) && out && *out) {
    void** vt = vtable_of(*out);
    ov_hook(vt, 16, reinterpret_cast<void*>(&ov_reset), g_reset);
    ov_hook(vt, 17, reinterpret_cast<void*>(&ov_present9), g_present9);
  }
  return hr;
}

// --- windows -------------------------------------------------------------------
HWND make_window(const char* title) {
  WNDCLASSA wc{};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "F2AUTest";
  RegisterClassA(&wc);  // a second registration fails harmlessly
  return CreateWindowA("F2AUTest", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 480, nullptr, nullptr, wc.hInstance,
                       nullptr);
}

void pump() {
  MSG m;
  while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&m);
    DispatchMessageA(&m);
  }
}

// Another thread's window: a tool's throwaway, not the game's.
HWND g_other = nullptr;
HANDLE g_other_ready = nullptr;
DWORD g_other_tid = 0;
DWORD WINAPI other_thread(LPVOID) {
  g_other = make_window("other thread");
  SetEvent(g_other_ready);
  MSG m;
  while (GetMessageA(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageA(&m);
  }
  return 0;
}

// --- D3D9 ----------------------------------------------------------------------
IDirect3DDevice9* make_device(IDirect3D9* d3d, HWND w, D3DPRESENT_PARAMETERS* pp) {
  *pp = D3DPRESENT_PARAMETERS{};
  pp->Windowed = TRUE;
  pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp->BackBufferFormat = D3DFMT_UNKNOWN;
  pp->hDeviceWindow = w;
  IDirect3DDevice9* dev = nullptr;
  const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_SOFTWARE_VERTEXPROCESSING, pp, &dev);
  if (FAILED(hr)) std::printf("  CreateDevice -> 0x%08lX\n", hr);
  return SUCCEEDED(hr) ? dev : nullptr;
}

bool frame(IDirect3DDevice9* dev) {
  pump();
  dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(20, 20, 40), 1.0f, 0);
  const bool ok = SUCCEEDED(dev->BeginScene()) && SUCCEEDED(dev->EndScene());
  return SUCCEEDED(dev->Present(nullptr, nullptr, nullptr, nullptr)) && ok;
}

void game_device(const char* name, IDirect3D9* d3d, HWND w, void** cls) {
  char what[160];
  D3DPRESENT_PARAMETERS pp;
  IDirect3DDevice9* dev = make_device(d3d, w, &pp);
  std::snprintf(what, sizeof(what), "%s made", name);
  check(dev != nullptr, what);
  if (!dev) return;
  void** vt = vtable_of(dev);
  if (!cls) cls = vt;
  if (g_expect_adopt) {
    std::snprintf(what, sizeof(what), "%s has a vtable of its own, EndScene/Reset the proxy's", name);
    check(vt != cls && in_proxy(vt[42]) && in_proxy(vt[16]), what);
    std::snprintf(what, sizeof(what), "%s: the class's vtable holds no function of the proxy's", name);
    check(!in_proxy(cls[42]) && !in_proxy(cls[16]), what);
  } else {
    std::snprintf(what, sizeof(what), "%s shares the class's vtable, EndScene/Reset the proxy's", name);
    check(vt == cls && in_proxy(cls[42]) && in_proxy(cls[16]), what);
  }
  if (g_overlay) {
    std::snprintf(what, sizeof(what), "%s: the overlay's saved Reset/Present are not the proxy's", name);
    check(!in_proxy(g_reset.saved) && !in_proxy(g_present9.saved), what);
  }
  const int before_p = g_present9.calls, before_r = g_reset.calls;
  bool ok = true;
  for (int i = 0; i < 3; ++i) ok = frame(dev) && ok;
  std::snprintf(what, sizeof(what), "%s: 3 frames succeed%s", name, g_overlay ? ", each Present through the overlay once" : "");
  check(ok && !g_present9.looped && (!g_overlay || g_present9.calls - before_p == 3), what);
  const HRESULT hr = dev->Reset(&pp);
  std::snprintf(what, sizeof(what), "%s: Reset succeeds%s (0x%08lX)", name, g_overlay ? ", through the overlay once" : "", hr);
  check(SUCCEEDED(hr) && !g_reset.looped && (!g_overlay || g_reset.calls - before_r == 1), what);
  std::snprintf(what, sizeof(what), "%s: a frame after the Reset succeeds", name);
  check(frame(dev) && !g_present9.looped, what);
  dev->Release();
}

int run() {
  if (g_overlay) {
    // The overlay is in before the proxy: its CreateDevice hook on the IDirect3D9 class.
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
      std::printf("SKIP: no Direct3D 9\n");
      return 0;
    }
    g_real_create_device = vtable_of(d3d)[16];
    write_slot(vtable_of(d3d), 16, reinterpret_cast<void*>(&ov_create_device));
    d3d->Release();
  }
  void* volatile* slot = import_slot("d3d9.dll", "Direct3DCreate9");
  check(slot != nullptr, "this exe imports Direct3DCreate9");
  if (!slot) return 1;
  g_proxy = LoadLibraryA("LTMemory.dll");
  check(g_proxy != nullptr, "proxy loaded");
  if (!g_proxy) return 1;
  // The proxy's mod thread patches the import.
  for (int i = 0; i < 500 && !in_proxy(*slot); ++i) Sleep(10);
  check(in_proxy(*slot), "the proxy's mod thread hooked the Direct3DCreate9 import");
  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) return 1;
  check(in_proxy(vtable_of(d3d)[16]), "IDirect3D9's CreateDevice is the proxy's (in place)");
  HWND w = make_window("game window");
  D3DPRESENT_PARAMETERS pp;
  IDirect3DDevice9* other = make_device(d3d, g_other, &pp);
  check(other != nullptr, "device for another thread's window made");
  void** cls = other ? vtable_of(other) : nullptr;
  if (other && g_expect_adopt) check(!in_proxy(cls[42]) && !in_proxy(cls[16]), "another thread's device keeps its class's vtable");
  if (other) check(frame(other) && !g_present9.looped, "another thread's device presents");
  game_device("device 1", d3d, w, cls);
  game_device("device 2", d3d, w, cls);  // a second device: the overlay's second pass
  if (other) {
    check(frame(other) && !g_present9.looped, "another thread's device still presents");
    other->Release();
  }
  d3d->Release();
  DestroyWindow(w);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::printf("usage: test_adopt.exe <dir with LTMemory.dll + LTMemory_orig.dll> overlay|none adopt|class\n");
    return 2;
  }
  const char* dir = argv[1];
  g_overlay = std::strcmp(argv[2], "overlay") == 0;
  g_expect_adopt = std::strcmp(argv[3], "adopt") == 0;
  SetDllDirectoryA(dir);
  SetCurrentDirectoryA(dir);
  // The proxy's ini: the panel drawn on every frame, no game to hook.
  char ini[MAX_PATH];
  std::snprintf(ini, sizeof(ini), "%sFear2AwardUnlocker.ini", dir);
  if (FILE* f = std::fopen(ini, "w")) {
    std::fputs("ShowOnStart = 1\nDisable = callbacks,achievements\n", f);
    std::fclose(f);
  }
  g_other_ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  HANDLE t = CreateThread(nullptr, 0, other_thread, nullptr, 0, &g_other_tid);
  WaitForSingleObject(g_other_ready, 5000);
  std::printf("D3D9, %s, expecting %s:\n", g_overlay ? "the Steam overlay's way of hooking" : "no overlay",
              g_expect_adopt ? "a vtable of its own" : "the class hook");
  const int rc = run();
  PostThreadMessageA(g_other_tid, WM_QUIT, 0, 0);
  WaitForSingleObject(t, 5000);
  if (g_reset.looped || g_present9.looped)
    std::printf("  LOOP: the overlay's hook and the proxy's called each other (a stack overflow in the game)\n");
  std::printf("%s (%d failure(s))\n", g_failures || rc ? "FAILED" : "PASSED", g_failures);
  std::fflush(stdout);
  return g_failures || rc ? 1 : 0;  // ExitProcess: the proxy's DllMain sees a process exit
}
