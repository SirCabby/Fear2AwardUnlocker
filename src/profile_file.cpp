#include "profile_file.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "log.h"

namespace f2au::profile {
namespace {

char g_path[MAX_PATH] = {};
bool g_backed_up = false;

// Read ProfileName= out of Game.ini; the game defaults to Profile000.
void read_profile_name(const char* dir, char* out, size_t n) {
  std::snprintf(out, n, "Profile000");
  char ini[MAX_PATH];
  std::snprintf(ini, sizeof(ini), "%s\\Game.ini", dir);
  FILE* f = std::fopen(ini, "r");
  if (!f) return;
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    const char* key = "ProfileName=";
    char* p = std::strstr(line, key);
    if (!p) continue;
    p += std::strlen(key);
    char* e = p + std::strlen(p);
    while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = '\0';
    if (*p) std::snprintf(out, n, "%s", p);
    break;
  }
  std::fclose(f);
}

}  // namespace

const char* path() { return g_path; }

bool locate() {
  if (g_path[0]) return true;

  char docs[MAX_PATH] = {};
  if (SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs) != S_OK) {
    logf("profile: SHGetFolderPathA(Documents) failed");
    return false;
  }

  char root[MAX_PATH];
  std::snprintf(root, sizeof(root), "%s\\WBGames\\FEAR2", docs);

  // The per-user folder is named with the SteamID, so discover it rather than
  // hardcoding; take the first real subdirectory.
  char pattern[MAX_PATH];
  std::snprintf(pattern, sizeof(pattern), "%s\\*", root);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    logf("profile: no user folder under %s", root);
    return false;
  }
  char user[MAX_PATH] = {};
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    if (!std::strcmp(fd.cFileName, ".") || !std::strcmp(fd.cFileName, "..")) continue;
    std::snprintf(user, sizeof(user), "%s\\%s", root, fd.cFileName);
    break;
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  if (!user[0]) {
    logf("profile: no user subfolder under %s", root);
    return false;
  }

  char name[64];
  read_profile_name(user, name, sizeof(name));
  std::snprintf(g_path, sizeof(g_path), "%s\\Profiles\\%s.gps", user, name);

  if (GetFileAttributesA(g_path) == INVALID_FILE_ATTRIBUTES) {
    logf("profile: %s does not exist", g_path);
    g_path[0] = '\0';
    return false;
  }
  logf("profile file: %s", g_path);
  return true;
}

void backup_once() {
  if (g_backed_up || !g_path[0]) return;
  g_backed_up = true;
  char bak[MAX_PATH];
  std::snprintf(bak, sizeof(bak), "%s.bak", g_path);
  if (GetFileAttributesA(bak) != INVALID_FILE_ATTRIBUTES) return;  // keep the first one
  if (CopyFileA(g_path, bak, TRUE))
    logf("profile: backed up to %s", bak);
  else
    logf("profile: WARNING could not write %s (error %lu)", bak, GetLastError());
}

bool set(uint32_t crc, int32_t value) {
  if (!g_path[0] && !locate()) return false;
  backup_once();

  FILE* f = std::fopen(g_path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 12) {
    std::fclose(f);
    return false;
  }
  std::vector<unsigned char> buf(static_cast<size_t>(size));
  size_t got = std::fread(buf.data(), 1, buf.size(), f);
  std::fclose(f);
  if (got != buf.size()) return false;

  uint32_t count = 0;
  std::memcpy(&count, buf.data() + 8, 4);

  size_t off = 12;
  bool patched = false;
  for (uint32_t i = 0; i < count && off + 8 <= buf.size(); ++i) {
    uint32_t key = 0, len = 0;
    std::memcpy(&key, buf.data() + off, 4);
    std::memcpy(&len, buf.data() + off + 4, 4);
    off += 8;
    if (off + len > buf.size()) return false;  // malformed; do not write anything
    if (key == crc && (len == 1 || len == 4)) {
      // Records are stored at their natural width; a flag is one byte and
      // writing four would run over the next record's key.
      if (len == 1)
        buf[off] = static_cast<unsigned char>(value);
      else
        std::memcpy(buf.data() + off, &value, 4);
      patched = true;
    }
    off += len;
  }
  if (off != buf.size()) {
    logf("profile: %s did not parse cleanly (%zu of %zu bytes) - not writing", g_path, off,
         buf.size());
    return false;
  }
  if (!patched) return false;

  // Write via a temp file in the same directory and rename over the target, so
  // an interrupted write cannot leave a truncated profile behind.
  char tmp[MAX_PATH];
  std::snprintf(tmp, sizeof(tmp), "%s.f2au-tmp", g_path);
  FILE* out = std::fopen(tmp, "wb");
  if (!out) return false;
  bool ok = std::fwrite(buf.data(), 1, buf.size(), out) == buf.size();
  std::fclose(out);
  if (!ok) {
    DeleteFileA(tmp);
    return false;
  }
  if (!MoveFileExA(tmp, g_path, MOVEFILE_REPLACE_EXISTING)) {
    logf("profile: rename over %s failed (error %lu)", g_path, GetLastError());
    DeleteFileA(tmp);
    return false;
  }
  return true;
}

}  // namespace f2au::profile
