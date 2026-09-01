#pragma once

namespace f2au::overlay {

// Hooks Direct3DCreate9 in the game's import table so we can capture the device
// as soon as the renderer is created. Safe to call before the renderer exists.
bool install();

// Undo the window-procedure and Direct3D patches before the DLL unloads.
void uninstall();

// Called the moment the game announces it is quitting (Global.Exit), while the
// device and window are still valid. Releasing ImGui's Direct3D resources here
// - rather than leaving them alive until the DLL detaches - is what stops the
// game tearing down a device that still has our objects referencing it.
void on_game_exiting();

}  // namespace f2au::overlay
