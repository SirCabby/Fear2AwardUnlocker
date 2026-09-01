#pragma once

#include <cstdint>

// Offsets into Game/gameclient.dll, expressed as RVAs against its preferred
// image base of 0x10000000. The module is relocated at load (observed at
// 0x02d80000), so these are only ever used as module_base + RVA.
//
// Every value below was read out of the shipped binary and then re-checked
// against live process memory.
namespace f2au::gc {

inline constexpr uintptr_t kPreferredBase = 0x10000000;

// vtable of the achievement manager class (ctor at RVA 0x16E2C0).
inline constexpr uintptr_t kAchievementVTable = 0x1C9738;

// Slot 2:  BuildAchievementList  (RVA 0x16E050) __thiscall, ret 8
// Slot 7:  GrantAchievement      (RVA 0x16D6D0) __thiscall, ret 4
// Slot 8:  IsAchievementComplete (RVA 0x16D7B0) __thiscall, ret 4 -> bool in AL
inline constexpr int kSlotBuildList = 2;
inline constexpr int kSlotGrant = 7;
inline constexpr int kSlotIsComplete = 8;

inline constexpr uintptr_t kBuildAchievementList = 0x16E050;
inline constexpr uintptr_t kGrantAchievement = 0x16D6D0;
inline constexpr uintptr_t kIsAchievementComplete = 0x16D7B0;

// Globals.
inline constexpr uintptr_t kAchievementMgr = 0x1F64B0;  // ptr to the manager
// The profile-data object is not a plain global. IsAchievementComplete reaches
// it as:   obj = call_51FE0(this = [0x101F9E30] + 0x98, arg = [self + 0x14]);
//          profile_data = obj + 0x86C;
// and that profile_data is what owns the record vector (see kRecords* below).
inline constexpr uintptr_t kProfileOwnerGlobal = 0x1F9E30;
inline constexpr uintptr_t kProfileOwnerOffset = 0x98;
inline constexpr uintptr_t kResolveProfile = 0x51FE0;   // __thiscall, 1 arg
inline constexpr uintptr_t kProfileDataOffset = 0x86C;

// Layout of the profile-data object, from FindRecordIndex (RVA 0x825E0):
//   +0x18  begin pointer of a record vector
//   +0x1C  end pointer
// Records are 20 bytes: value at +0x00, crc32 key at +0x10.
inline constexpr uintptr_t kRecordsBegin = 0x18;
inline constexpr uintptr_t kRecordsEnd = 0x1C;
inline constexpr int kRecordStride = 20;
inline constexpr int kRecordKeyOffset = 0x10;

// The achievement records IsAchievementComplete is called with are contiguous,
// 0x18 bytes apart, one per Interface/Achievements row in database order.
inline constexpr int kAchievementRecordStride = 0x18;
inline constexpr uintptr_t kUnlockAchievementsVar = 0x1F9E8C;  // console var handle

// PushAchievementsToFlash feeds Menu.AddAchievementInfo one row per award. It
// has exactly one call site, so we redirect that rather than detour it. Note
// that it carries only AchievementIndex/Name/Description/Points - the rows, not
// their state.
inline constexpr uintptr_t kPushAchievementsToFlash = 0x00A470;
inline constexpr uintptr_t kPushAchievementsCallSite = 0x00B225;

// Completion goes to the menu separately, and this is the function that sends
// it: one Menu.AchievementClear, then one Menu.AchievementSetProgress(id,
// progress, required) per award, in database order. It reads each award's state
// through vtable slot 8 of the achievement manager embedded at +0x828 on the
// profile owner - the very slot this mod hooks - so calling it again re-pushes
// whatever the toggles now say. __cdecl(menu, profileData); the second argument
// is unused by this function (the game passes it for the missions half of the
// same refresh).
inline constexpr uintptr_t kPushAchievementProgress = 0x00A130;
inline constexpr uintptr_t kAchievementClearCall = 0x00A144;    // its first call
inline constexpr uintptr_t kMenuAchievementClear = 0x009D50;    // ... to this
inline constexpr uintptr_t kAchievementClearNameImm = 0x009D66;  // push "AchievementClear"
inline constexpr uintptr_t kAchievementClearNameStr = 0x1C2B48;

// The push needs the live menu object, and there is not always one: the game's
// own refresh path (RVA 0xAFC0) asks the menu-manager singleton for it and does
// nothing when the answer is null. __thiscall(self) -> menu or null.
inline constexpr uintptr_t kMenuManagerSingleton = 0x1FD960;
inline constexpr uintptr_t kGetActiveMenu = 0x02FAF0;

// ActionScript -> C++ bridge. The menu registers named callbacks in a table of
// 12-byte {const char* name, void* handler, u8 flag} rows. The one we want is
// "Menu.StateChanged", which Flash calls on every menu screen change with a
// single numeric argument identifying the new screen - the only notification
// the game gives that the player has navigated somewhere.
//
//   [0x101F6B80] = "Menu.StateChanged"
//   [0x101F6B84] = handler (0x100094A0), __stdcall (void*, GFxValue* args, int argc)
//
// Patching the handler pointer is a clean data swap, no code patching needed.
inline constexpr uintptr_t kMenuStateChangedName = 0x1F6B80;
inline constexpr uintptr_t kMenuStateChangedHandler = 0x1F6B84;
inline constexpr uintptr_t kMenuStateChangedExpected = 0x0094A0;
inline constexpr uintptr_t kMenuStateChangedNameStr = 0x1C2FF0;

// The handler reads args[0] as a Scaleform value: type tag at +0x00 (3 = number)
// and the double payload at +0x08.
inline constexpr int kGfxValueTypeNumber = 3;
inline constexpr uintptr_t kGfxValueNumberOffset = 0x08;

// FindAchievement(mgr, id, 0) -> achievement object, RVA 0x11D5E0.
inline constexpr uintptr_t kFindAchievement = 0x11D5E0;

// Localization. BuildAchievementList turns a loc key into display text with:
//     mgr = [0x1020459C]; ctx = [0x102045A0];
//     if (mgr && ctx && key && *key) text = mgr->vtbl[8](ctx, key);
// (vtable byte offset 0x20). Returns a wide string; on failure the game
// substitutes the literals "" or "[".
inline constexpr uintptr_t kStringMgr = 0x20459C;
inline constexpr uintptr_t kStringCtx = 0x2045A0;
inline constexpr int kLocalizeVtableSlot = 8;

// Conditions block sits at +0x86C on the achievement object.
inline constexpr uintptr_t kConditionsOffset = 0x86C;

}  // namespace f2au::gc
