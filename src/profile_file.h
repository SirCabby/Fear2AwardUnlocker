#pragma once

#include <cstdint>

// Reading and writing Profile000.gps on disk.
//
// The mod changes award records in the game's in-memory profile, but the game
// only flushes that to disk at moments of its own choosing - quitting with kill
// (or a crash) loses it. So every toggle is written to BOTH the live memory and
// the file. They carry the same value, so whichever copy the game writes last,
// the result agrees.
//
// File format (no checksum, no signature):
//     u32 version (17)   u32 version (17)   u32 recordCount
//     recordCount x { u32 crc32(name), u32 valueLen, <valueLen bytes LE> }
// Both version fields are validated by the game; a mismatch makes it discard
// the whole profile, so they are preserved untouched.
namespace f2au::profile {

// Locates <Documents>\WBGames\FEAR2\<steamid>\Profiles\<ProfileName>.gps.
// Returns false if it cannot be found.
bool locate();
const char* path();

// Copies the .gps to .gps.bak once, before the mod's first ever write.
void backup_once();

// Patch one existing record in the file, using whatever width that record
// already has - 1 byte for a flag, 4 for an int32. Returns false if the file is
// missing, malformed, or has no record with that key.
bool set(uint32_t crc, int32_t value);

}  // namespace f2au::profile
