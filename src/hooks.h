#pragma once

namespace f2au::hooks {

// Waits for Game/gameclient.dll, then installs the achievement hooks.
// Returns false if the module never showed up.
bool install();

// Undo every patch, so nothing points into this DLL once it unloads.
void uninstall();

// True once, each time the game finishes sweeping every award - which is how
// the overlay knows the Awards screen has just been (re)built.
bool consume_sweep_event();

// Called each frame so a sweep that has gone quiet can be closed out.
void poll();

// The menu screen id Flash last reported through Menu.StateChanged, or -1 if it
// has not reported one yet.
int menu_state();

}  // namespace f2au::hooks
