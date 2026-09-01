#pragma once

#include <cstdint>

namespace f2au::awards {

// What the mod is forcing an award to, independent of what the game thinks.
enum class Forced : uint8_t {
  kDefault = 0,  // leave the game's own answer alone
  kOn,           // report complete
  kOff,          // report incomplete
};

// --- mod state -------------------------------------------------------------
Forced forced(int index);
void set_forced(int index, Forced value);
int index_of(const char* award_id);  // -1 if unknown

// What the game itself last reported for an award, cached by the hook so the UI
// can show a meaningful state for awards it is not forcing.
void set_observed(int index, bool complete);
bool observed(int index);

// The state the player actually sees: the forced value if any, else the last
// value the game reported.
bool effective(int index);

// Settings, also read from the config file.
struct Settings {
  int toggle_key = 0x77;      // VK_F8
  bool show_on_start = false;
  bool auto_show = true;      // open the panel when the Awards screen appears
  int sort_mode = 0;          // see overlay.cpp kSortModes
  bool trace_menu_callbacks = false;  // log every ActionScript -> C++ menu callback
  // Diagnostic switches: "Disable = overlay,callbacks,achievements" turns whole
  // subsystems off so a shutdown fault can be bisected to one of them.
  bool disable_overlay = false;
  bool disable_callbacks = false;
  bool disable_achievements = false;
};
const Settings& settings();
Settings& mutable_settings();

// Settings file next to the DLL. Award state is not in it - that lives in the
// game's own profile - so this is only the panel's own preferences.
void load_config(const char* dir);
void save_config();

// --- the game's profile ----------------------------------------------------
// `profile_data` is the object that owns the 20-byte key/value record vector;
// hooks.cpp resolves it and hands it over.
void set_profile_data(void* profile_data);
void* profile_data();

// Write one of the profile's one-byte flag records - which is what an award is.
// False if the record is not there; creating one needs the game's own allocator.
bool profile_set_flag(uint32_t crc, bool value);

// Apply a toggle. Every award, the Reborn DLC ones included, owns one byte in
// the profile - the same byte GrantAchievement writes when the award is earned
// for real - so a toggle writes that byte in both the live profile and the file.
// The change is the game's own state: it persists, and it outlives the mod.
// Returns true if the byte was written somewhere.
bool apply(int index, bool complete);

// Re-apply everything currently toggled. The profile only becomes reachable once
// the game is far enough along to resolve it, so this flushes anything that was
// set before then. Returns how many awards were written.
int apply_all_forced();

}  // namespace f2au::awards
