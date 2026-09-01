#pragma once

// Minimal file logger. The mod runs inside a 2009 game under Proton, where a
// debugger is awkward and stdout goes nowhere, so a log file beside the DLL is
// the primary diagnostic channel.
namespace f2au {

// Called once from DllMain with the directory the DLL was loaded from.
void log_init(const char* dir);
void log_shutdown();

void logf(const char* fmt, ...);

}  // namespace f2au
