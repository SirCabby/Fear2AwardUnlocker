#pragma once

#include <windows.h>

// Private vtables for the renderer object the panel hooks (the game's D3D9
// device), so that no function of the mod's sits in a vtable another hooker
// reads. Windows only, and only for a class's vtable (a device with one of its
// own keeps it - overlay.cpp): under Wine the class vtable is hooked as before.
// See adopt.cpp for why.
namespace f2au::adopt {

// From DllMain: records the thread that loaded the mod (the game's primary
// thread, which makes its window) and the mod's image, and decides the way.
void init();

// True on Windows (or with F2AU_ADOPT set in the environment, for the test).
bool enabled();

// A window of this process made by the thread that loaded the mod: the game's.
bool game_window(HWND w);

// "window 00100760 (class 'X', thread 1234)" for log lines.
void describe_window(HWND w, char* out, size_t n);

// Logs (the first few) objects made for a window that is not the game's.
void leave_alone(const char* what, const char* how, HWND w);

struct Hook {
  int slot;
  void* fn;
};

// Points `obj` at a private copy of its vtable with `hooks` in it: `want`
// slots when the read allows, never fewer than `least` (the interface's). An
// object that already has its copy is left as it is; a new one the game made
// at a freed one's address is adopted afresh. Returns the slots copied (0: not
// adopted, the object untouched).
int take(void* obj, int least, int want, const Hook* hooks, int nhooks);

// What the hook in `slot` of an adopted object calls on to (see adopt.cpp), or
// `fallback` for an object that was not adopted (the class hook's original).
void* original(const void* obj, int slot, void* fallback);

// Puts the class's functions back into every adopted object's copy in place of
// the mod's (the objects keep their copies): the mod's hooks are out, as when
// the class hook's slots are restored.
void unhook_all();

// A hook's call on to the object's own function is under way on this thread:
// a hook of another tool's that calls the object's vtable again has come back.
bool calling_on();

struct CallingOn {
  CallingOn();
  ~CallingOn();
  CallingOn(const CallingOn&) = delete;
  CallingOn& operator=(const CallingOn&) = delete;
};

}  // namespace f2au::adopt
