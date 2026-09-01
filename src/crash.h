#pragma once

namespace f2au {

// Install a last-resort exception filter that records where a fatal fault
// happened, and in which module. Injected mods are the usual suspect for
// crashes at shutdown, so it needs to be possible to tell from the log whether
// the fault is in this DLL or somewhere else entirely.
void install_crash_logger();

// Hook MSVCR80's _purecall in the game modules so an R6025 records where it
// came from instead of only showing a dialog. Call once gameclient.dll exists.
void install_purecall_logger();

}  // namespace f2au
