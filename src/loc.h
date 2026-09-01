#pragma once

namespace f2au::loc {

// Resolve a Loki.strdb loc key ("Achievement_Laser_NAME") to the game's own
// display text, in whatever language the player is running, and return it as
// UTF-8 for ImGui. Returns nullptr if the string manager is not up yet or the
// key has no entry.
//
// This calls the game's own localizer rather than parsing Loki.strdb, so it is
// always in sync with the game and needs no knowledge of the file format.
const char* text(const char* key);

}  // namespace f2au::loc
