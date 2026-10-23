// The Awards-screen inference (src/callbacks.cpp) fed the call sequences the menu's ActionScript
// makes - see CLAUDE.md, "Menu screen detection" - at 60, 30 and 15 frames a second:
//
//   mkdir -p tests/build
//   i686-w64-mingw32-g++ -std=c++20 -O2 -Wall -DWIN32_LEAN_AND_MEAN -static -Isrc
//       tests/test_menu_nav.cpp -o tests/build/test_menu_nav.exe
//   wine tests/build/test_menu_nav.exe        (about half a minute: it runs on the real clock)
//
// The test includes callbacks.cpp itself, to reach the state machine, with the few functions it
// calls from elsewhere stubbed. The 1.0.1 inference fails it: after the first-run brightness
// screen, and on Options and New Game below 60 fps.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "awards.h"
#include "log.h"
#include "overlay.h"

namespace f2au {
void logf(const char* fmt, ...) {
  std::va_list ap;
  va_start(ap, fmt);
  std::printf("      log: ");
  std::vprintf(fmt, ap);
  std::printf("\n");
  va_end(ap);
}
namespace awards {
const Settings& settings() {
  static Settings s;
  return s;
}
}  // namespace awards
namespace overlay {
void on_game_exiting() {}
}  // namespace overlay
}  // namespace f2au

#include "callbacks.cpp"

using namespace f2au::callbacks;

namespace {

// A Scaleform value as the handlers get it: the type at +0, the payload at +8.
struct GfxValue {
  int type;
  int pad;
  union {
    double number;
    const char* str;
  };
};

int index_of(const char* name) {
  for (int i = 0; i < f2au::kMenuCallbackCount; ++i)
    if (!std::strcmp(f2au::kMenuCallbacks[i].name, name)) return i;
  std::printf("no callback %s\n", name);
  std::exit(2);
}

void call(const char* name) { note_navigation(index_of(name), nullptr, 0); }
void call_number(const char* name, double n) {
  GfxValue v{};
  v.type = 3;
  v.number = n;
  note_navigation(index_of(name), &v, 1);
}
void sound(const char* s) {
  GfxValue v{};
  v.type = 4;
  v.str = s;
  note_navigation(index_of("Global.PlaySound"), &v, 1);
}

// One menu frame: LokiMenuEvents.OnUpdate's timer, whatever the frame calls, then the render.
DWORD g_frame_ms = 16;
void frame(const char* also = nullptr) {
  call("Global.GetAccumulatedTime");
  if (also) call(also);
  Sleep(g_frame_ms);
  poll();
}
void frames(int n, const char* also = nullptr) {
  for (int i = 0; i < n; ++i) frame(also);
}

int g_failures = 0;
void expect(bool awards, const char* what) {
  const bool ok = on_awards_screen() == awards;
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

// The main menu comes up: its fade, then OnActivate.
void main_menu_up() {
  frames(20);
  call_number("Menu.SetReturnMenu", 1);
  frames(5);
}

// Picking a main-menu item: CButton.HandleSelect runs the callback (which may call the game),
// then plays the select sound.
void pick(const char* callback_calls = nullptr) {
  if (callback_calls) call(callback_calls);
  sound("menu_select");
}

// MenuManagers.CMainMenu's screen change: 300 ms of fade, a frame to finish it, the frame of the
// new screen's OnActivate, then its OnUpdate every frame.
void screen_change(const char* on_activate, double layer, const char* on_update) {
  frames(static_cast<int>((300 + g_frame_ms - 1) / g_frame_ms));
  frame();
  call("Global.GetAccumulatedTime");
  if (on_activate) call_number(on_activate, layer);
  Sleep(g_frame_ms);
  poll();
  frames(15, on_update);
}

void back_to_main_menu() {
  sound("menu_back");
  screen_change("Menu.SetReturnMenu", 1, nullptr);
}

// A key press: after the menu's own handling of it, every text box's key listener
// (MenuComponents.CInput) calls Menu.CancelAttractMode - 11 of them, traced on the laptop.
void key_burst() {
  for (int i = 0; i < 11; ++i) call("Menu.CancelAttractMode");
}

}  // namespace

int main() {
  for (int fps : {60, 30, 15}) {
    g_frame_ms = 1000 / fps;
    g_nav = Nav::kUnknown;
    std::printf("%d frames a second\n", fps);

    frames(10);
    sound("menu_select");  // the first-run brightness screen, confirmed before the main menu
    frames(3);
    main_menu_up();
    expect(false, "main menu after the brightness screen: no panel");

    pick();
    screen_change(nullptr, 0, nullptr);
    expect(true, "Awards: the panel");
    frames(30);
    expect(true, "Awards, 30 frames on: the panel");
    sound("menu_back");
    expect(false, "backing out of Awards: no panel");
    screen_change("Menu.SetReturnMenu", 1, nullptr);
    expect(false, "the main menu again: no panel");

    pick();
    frame("Menu.CancelAttractMode");  // once, during the fade
    frame();
    screen_change(nullptr, 0, nullptr);
    expect(true, "Awards with a lone CancelAttractMode during the fade: the panel");
    back_to_main_menu();
    pick();
    screen_change(nullptr, 0, nullptr);
    expect(true, "Awards before a lone CancelAttractMode: the panel");
    call("Menu.CancelAttractMode");
    frames(10);
    expect(true, "Awards after it: still the panel");
    back_to_main_menu();
    pick();
    frames(static_cast<int>((300 + g_frame_ms - 1) / g_frame_ms));
    frame("Menu.CancelAttractMode");  // once, as the screen comes up
    frames(15);
    expect(true, "Awards with a lone CancelAttractMode as it comes up: the panel");
    back_to_main_menu();

    std::printf("  with the keyboard (laptop trace, 2026-10-22)\n");
    key_burst();  // the arrow key that moved onto Awards, a frame earlier
    frame();
    pick();
    key_burst();  // Enter's own burst, right after the select sound
    screen_change(nullptr, 0, nullptr);
    expect(true, "Awards picked with Enter: the panel");
    back_to_main_menu();
    pick();
    key_burst();
    frames(2);
    key_burst();  // arrow keys pressed while it waits, a few frames apart
    frames(3);
    key_burst();
    screen_change(nullptr, 0, nullptr);
    expect(true, "Awards with keys pressed during the fade: the panel");
    back_to_main_menu();
    pick();
    key_burst();
    screen_change(nullptr, 0, "Menu.CancelAttractMode");
    expect(false, "Options picked with Enter: no panel");
    back_to_main_menu();

    pick();
    screen_change(nullptr, 0, "Menu.CancelAttractMode");
    expect(false, "Options (CancelAttractMode every OnUpdate): no panel");
    back_to_main_menu();

    pick("Multiplayer.IsConnectedOnline");
    screen_change("Menu.SetReturnMenu", 10, nullptr);
    expect(false, "New Game: no panel");
    back_to_main_menu();
    pick();
    screen_change("Menu.SetReturnMenu", 10, nullptr);
    expect(false, "New Game with nothing called before the sound: no panel");
    back_to_main_menu();

    pick("SaveGame.LoadCheckpointSave");
    frames(40);
    expect(false, "Continue, the menu running on: no panel");
    main_menu_up();
    pick();
    Sleep(3200);  // no menu frames: a level loading
    poll();
    frames(10);
    expect(false, "a selection after which the menu stopped: no panel");

    main_menu_up();
    pick("Global.Exit");
    frames(30);
    expect(false, "Quit: no panel");
  }
  std::printf("%s (%d failure(s))\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
