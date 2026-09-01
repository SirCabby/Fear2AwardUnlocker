#pragma once

namespace f2au::callbacks {

// Hook every ActionScript -> C++ menu callback so we can see which ones the
// game actually invokes as the player moves around the menus. This is how the
// Awards screen gets detected: F.E.A.R. 2 fires no achievement-related code when
// that screen opens, so the only way to notice it is to watch the whole bridge.
bool install(unsigned long base);

// Name of the callback most recently invoked, or nullptr.
const char* last();

// Best estimate of whether the player is sitting on the game's Awards screen.
// See the state machine in callbacks.cpp for how this is inferred.
bool on_awards_screen();

// Advance time-based parts of that estimate; call once per frame.
void poll();

// Put every patched handler pointer back. Must run before the DLL unloads,
// otherwise the table still points into our unmapped image.
void uninstall(unsigned long base);

}  // namespace f2au::callbacks
