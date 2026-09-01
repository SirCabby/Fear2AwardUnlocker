#include "awards.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "awards_table.h"
#include "gameclient.h"
#include "log.h"
#include "mem.h"
#include "profile_file.h"

namespace f2au::awards {
namespace {

Forced g_forced[kAwardCount] = {};
bool g_observed[kAwardCount] = {};
Settings g_settings;
void* g_profile = nullptr;
char g_config_path[MAX_PATH] = {};

// Walk the profile's record vector for a key. The vector is small (336 entries
// on a normal profile) and lookups only happen on a toggle or a screen rebuild,
// so a linear scan is fine and avoids caching pointers that the game may move.
uintptr_t find_record(uint32_t crc) {
  if (!g_profile) return 0;
  auto base = reinterpret_cast<uintptr_t>(g_profile);
  auto begin = mem::read<uintptr_t>(base + gc::kRecordsBegin);
  auto end = mem::read<uintptr_t>(base + gc::kRecordsEnd);
  if (!begin || end <= begin) return 0;
  if ((end - begin) % gc::kRecordStride) return 0;  // not the layout we expect
  for (uintptr_t r = begin; r < end; r += gc::kRecordStride)
    if (mem::read<uint32_t>(r + gc::kRecordKeyOffset) == crc) return r;
  return 0;
}

}  // namespace

const Settings& settings() { return g_settings; }
Settings& mutable_settings() { return g_settings; }

Forced forced(int index) {
  return (index >= 0 && index < kAwardCount) ? g_forced[index] : Forced::kDefault;
}

void set_forced(int index, Forced value) {
  if (index >= 0 && index < kAwardCount) g_forced[index] = value;
}

int index_of(const char* award_id) {
  for (int i = 0; i < kAwardCount; ++i)
    if (std::strcmp(kAwards[i].id, award_id) == 0) return i;
  return -1;
}

void set_observed(int index, bool complete) {
  if (index >= 0 && index < kAwardCount) g_observed[index] = complete;
}

bool observed(int index) {
  return (index >= 0 && index < kAwardCount) ? g_observed[index] : false;
}

bool effective(int index) {
  switch (forced(index)) {
    case Forced::kOn: return true;
    case Forced::kOff: return false;
    default: return observed(index);
  }
}

void set_profile_data(void* p) { g_profile = p; }
void* profile_data() { return g_profile; }

bool profile_set_flag(uint32_t crc, bool value) {
  uintptr_t r = find_record(crc);
  if (!r) return false;  // record does not exist; appending needs the game's allocator
  // A flag record holds one byte; the three bytes above it belong to the game.
  return mem::write<uint8_t>(r, value ? 1 : 0);
}

bool apply(int index, bool complete) {
  if (index < 0 || index >= kAwardCount) return false;
  const AwardDef& a = kAwards[index];
  set_forced(index, complete ? Forced::kOn : Forced::kOff);

  // Write both copies. The game flushes its in-memory profile at moments of its
  // own choosing, so memory alone can be lost; the file alone can be overwritten
  // by that flush. Writing the same value to both makes either outcome correct.
  const bool in_mem = profile_set_flag(a.storage_crc, complete);
  const bool on_disk = profile::set(a.storage_crc, complete ? 1 : 0);
  logf("apply %-28s -> %-3s  Achievements%-26s memory:%s file:%s", a.id,
       complete ? "on" : "off", a.storage, in_mem ? "ok" : "absent", on_disk ? "ok" : "absent");
  return in_mem || on_disk;
}

int apply_all_forced() {
  int written = 0;
  for (int i = 0; i < kAwardCount; ++i) {
    if (g_forced[i] == Forced::kDefault) continue;
    if (apply(i, g_forced[i] == Forced::kOn)) ++written;
  }
  return written;
}

void load_config(const char* dir) {
  std::snprintf(g_config_path, sizeof(g_config_path), "%sFear2AwardUnlocker.ini", dir);
  FILE* f = std::fopen(g_config_path, "r");
  if (!f) {
    logf("no config at %s (that is fine - the defaults are used)", g_config_path);
    return;
  }
  char line[256];
  int obsolete = 0;
  while (std::fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == ';' || *p == '[' || *p == '\n' || *p == '\r' || !*p) continue;
    char* eq = std::strchr(p, '=');
    if (!eq) continue;
    *eq = '\0';
    char* key = p;
    char* val = eq + 1;
    for (char* e = key + std::strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t');) *--e = '\0';
    while (*val == ' ' || *val == '\t') ++val;
    for (char* e = val + std::strlen(val);
         e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t');)
      *--e = '\0';

    if (!_stricmp(key, "ToggleKey")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v > 0 && v < 256) g_settings.toggle_key = v;
      continue;
    }
    if (!_stricmp(key, "AutoShow")) {
      g_settings.auto_show =
          !_stricmp(val, "1") || !_stricmp(val, "on") || !_stricmp(val, "true");
      continue;
    }
    if (!_stricmp(key, "Disable")) {
      g_settings.disable_overlay = std::strstr(val, "overlay") != nullptr;
      g_settings.disable_callbacks = std::strstr(val, "callbacks") != nullptr;
      g_settings.disable_achievements = std::strstr(val, "achievements") != nullptr;
      logf("config: Disable = %s (overlay=%d callbacks=%d achievements=%d)", val,
           g_settings.disable_overlay, g_settings.disable_callbacks,
           g_settings.disable_achievements);
      continue;
    }
    if (!_stricmp(key, "TraceMenuCallbacks")) {
      g_settings.trace_menu_callbacks =
          !_stricmp(val, "1") || !_stricmp(val, "on") || !_stricmp(val, "true");
      continue;
    }
    if (!_stricmp(key, "SortMode")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v >= 0 && v < 5) g_settings.sort_mode = v;
      continue;
    }
    if (!_stricmp(key, "ShowOnStart")) {
      g_settings.show_on_start = !_stricmp(val, "1") || !_stricmp(val, "on") ||
                                 !_stricmp(val, "true");
      continue;
    }

    // Award state used to live here, back when the mod believed the game had
    // nowhere to keep it. It does - one byte per award in the profile - so these
    // lines are ignored now, and dropped the next time the file is written.
    if (index_of(key) >= 0)
      ++obsolete;
    else
      logf("config: unknown key '%s' ignored", key);
  }
  std::fclose(f);
  if (obsolete)
    logf("config: ignored %d award line(s) left over from an older version of the mod - "
         "award state lives in the profile now",
         obsolete);
}

void save_config() {
  if (!g_config_path[0]) return;
  FILE* f = std::fopen(g_config_path, "w");
  if (!f) return;
  std::fprintf(f,
               "# Fear2AwardUnlocker\n"
               "#\n"
               "# Settings:\n"
               "#   ToggleKey   = 0x77      ; virtual-key code that opens the panel (0x77 = F8)\n"
               "#   ShowOnStart = 0         ; open the panel as soon as the game starts\n"
               "#   AutoShow    = 1         ; open the panel automatically on the Awards screen\n"
               "#   TraceMenuCallbacks = 0  ; diagnostics: log every ActionScript -> C++ menu call\n"
               "#   SortMode    = 0         ; 0 game order (A-Z, matches the game), 1 award index,\n"
               "#                           ; 2 points, 3 completed first, 4 database order\n"
               "#\n"
               "# Settings only. Award state is not kept here: the game stores one byte per\n"
               "# award in Profile000.gps, and that is what the panel writes.\n"
               "ToggleKey = 0x%02X\n"
               "ShowOnStart = %d\n"
               "AutoShow = %d\n"
               "SortMode = %d\n\n",
               g_settings.toggle_key, g_settings.show_on_start ? 1 : 0,
               g_settings.auto_show ? 1 : 0, g_settings.sort_mode);
  std::fclose(f);
}

}  // namespace f2au::awards
