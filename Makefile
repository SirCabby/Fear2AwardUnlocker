# Fear2AwardUnlocker - a 32-bit proxy-DLL mod for F.E.A.R. 2: Project Origin.
# Cross-compiled on Linux with mingw-w64 (no MSVC / no Windows needed).
#
# The mod ships as LTMemory.dll, standing in front of the game's own
# LTMemory.dll (renamed LTMemory_orig.dll by `make install`). FEAR2.exe imports
# exactly one symbol from it, which we forward. ltmemory is not a Wine builtin,
# so this needs no WINEDLLOVERRIDES under Proton - it just works on both.

CXX      := i686-w64-mingw32-g++

# The project name (used for the distributable) and the DLL name (fixed by the
# game, which imports LTMemory.dll) are deliberately different things.
PROJECT  := Fear2AwardUnlocker
NAME     := LTMemory

# VERSION holds the single source of truth. Change it with `make rev X.Y.Z`.
VERSION  := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
BUILD    := build
TARGET   := $(BUILD)/$(NAME).dll
DEF      := ltmemory.def

# Per-machine config (game install path), kept out of git. Copy
# config.mk.example to config.mk and set GAME_DIR there. Leading '-' means
# "don't error if absent", so `make` still builds with no local config.
-include config.mk

ifneq (,$(filter rev,$(MAKECMDGOALS)))
REV := $(strip $(filter-out rev,$(MAKECMDGOALS)))
$(eval $(REV):;@:)
endif

SRCS := $(wildcard src/*.cpp)

# Dear ImGui (vendored, MIT) supplies the overlay panel. Only the core plus the
# DX9/Win32 backends are compiled - no demo, no other backends.
IMGUI_DIR  := contrib/imgui
IMGUI_SRCS := $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp \
              $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp \
              $(IMGUI_DIR)/backends/imgui_impl_dx9.cpp \
              $(IMGUI_DIR)/backends/imgui_impl_win32.cpp

OBJS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRCS)) \
        $(patsubst $(IMGUI_DIR)/%.cpp,$(BUILD)/imgui/%.o,$(IMGUI_SRCS))

# -MMD -MP: emit .d header-dependency files so editing a header recompiles every
# .cpp that includes it.
CXXFLAGS := -m32 -std=c++20 -O2 -Wall -Wextra \
            -DWIN32_LEAN_AND_MEAN -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD \
            -I$(IMGUI_DIR) \
            -ffunction-sections -fdata-sections \
            -MMD -MP

# -static*: fold the C/C++ runtime in so the DLL has no external mingw runtime
# deps. Exports come from the .def, so no --exclude-all-symbols here.
LDFLAGS  := -m32 -shared \
            -static -static-libgcc -static-libstdc++ \
            -Wl,--gc-sections

LDLIBS   := -luser32 -lshell32 -lole32 -lgdi32 -ldwmapi

.PHONY: all clean install uninstall dist package rev version
all: $(TARGET)

$(TARGET): $(OBJS) $(DEF) | $(BUILD)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(DEF) $(LDLIBS)
	@echo ">> Built $@"

MOD_OBJS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRCS))

# Rebuild the mod's own objects when the version changes (the vendored ImGui
# objects do not use it, so they are left alone).
$(MOD_OBJS): VERSION

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -DF2AU_VERSION='"$(VERSION)"' -c $< -o $@

$(BUILD)/imgui/%.o: $(IMGUI_DIR)/%.cpp | $(BUILD)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)

$(BUILD):
	mkdir -p $(BUILD) $(BUILD)/imgui/backends

version:
	@echo $(VERSION)

# `make rev X.Y.Z` - set the version.
#
# Make has no argument syntax, so the new version arrives as a second goal. The
# block below defines a do-nothing target for it so make does not fail trying to
# build "1.2.3". It is scoped to invocations that actually asked for `rev`: a
# global match-anything rule would silently swallow mistyped target names.
rev:
	@v='$(REV)'; \
	if ! echo "$$v" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$$'; then \
	  echo "usage: make rev X.Y.Z        (for example: make rev 1.2.0)"; \
	  echo "current version: $(VERSION)"; \
	  exit 1; \
	fi; \
	printf '%s\n' "$$v" > VERSION; \
	echo ">> version $(VERSION) -> $$v"; \
	echo ">> run 'make package' to build $(PROJECT)_$$v.zip"

clean:
	rm -rf $(BUILD) $(DIST)

# --- Deploy -----------------------------------------------------------------
# The mod ships AS ltmemory.dll, so the stock one must first be moved aside to
# LTMemory_orig.dll (which our proxy forwards to). That rename happens exactly
# once and is guarded: if the live DLL is already our proxy but the original is
# missing, we refuse rather than enshrine the mod as the "stock" copy, which
# would permanently destroy the real DLL.
#
# FEAR2.exe imports the name spelled exactly 'LTMemory.dll', and on a
# case-sensitive filesystem Wine finds that exact file first - it only falls back
# to a case-insensitive scan when the literal name is missing. So the mod must be
# installed under that exact name, and no second case variant may be left lying
# around: an earlier version of this target copied the stock lowercase
# 'ltmemory.dll' aside and then wrote to it, leaving a stale 'LTMemory.dll' that
# silently kept loading in preference to every later install. Hence `mv` for the
# stock file, a fixed target name, and the sweep for leftovers at the end.
#
# Atomic install: write to a temp name in the SAME directory then rename(2) over
# the target, so a running game keeps its old intact mapping.
install: $(TARGET)
	@set -e; \
	if [ -z "$(GAME_DIR)" ]; then \
	  echo "ERROR: GAME_DIR is not set."; \
	  echo "  Copy config.mk.example to config.mk and set GAME_DIR,"; \
	  echo "  or run: make install GAME_DIR=/path/to/FEAR2"; \
	  exit 1; \
	fi; \
	if [ ! -d "$(GAME_DIR)" ]; then echo "ERROR: GAME_DIR '$(GAME_DIR)' does not exist."; exit 1; fi; \
	if [ ! -f "$(GAME_DIR)/FEAR2.exe" ]; then \
	  echo "ERROR: no FEAR2.exe in '$(GAME_DIR)' - is that really the game folder?"; exit 1; \
	fi; \
	orig="$(GAME_DIR)/LTMemory_orig.dll"; \
	live="$(GAME_DIR)/LTMemory.dll"; \
	if [ -f "$$orig" ]; then \
	  echo ">> stock original already preserved at $$orig"; \
	else \
	  stock=$$(ls "$(GAME_DIR)" | grep -ix 'ltmemory\.dll' | head -1); \
	  if [ -z "$$stock" ]; then echo "ERROR: no ltmemory.dll in $(GAME_DIR)."; exit 1; fi; \
	  if grep -aq "Fear2AwardUnlocker" "$(GAME_DIR)/$$stock" 2>/dev/null; then \
	    echo "ERROR: $(GAME_DIR)/$$stock is already our proxy, but $$orig is missing."; \
	    echo "       Refusing to save the mod as the 'stock' original."; \
	    echo "       Restore the real ltmemory.dll (Steam: Verify integrity of game files),"; \
	    echo "       then re-run make install."; \
	    exit 1; \
	  fi; \
	  mv -f "$(GAME_DIR)/$$stock" "$$orig"; \
	  echo ">> preserved stock $$stock -> LTMemory_orig.dll"; \
	fi; \
	t="$$live.f2au-new.$$$$"; cp -f $(TARGET) "$$t"; mv -f "$$t" "$$live"; \
	echo ">> installed (atomic) $$live"; \
	for stray in $$(ls "$(GAME_DIR)" | grep -ix 'ltmemory\.dll' | grep -vx 'LTMemory.dll'); do \
	  if grep -aq "Fear2AwardUnlocker" "$(GAME_DIR)/$$stray" 2>/dev/null; then \
	    rm -f "$(GAME_DIR)/$$stray"; \
	    echo ">> removed stale copy $$stray - it shadowed this install"; \
	  else \
	    echo ">> WARNING: $$stray exists and is not ours - it may shadow the mod"; \
	  fi; \
	done; \
	echo ">> NOTE: Steam 'Verify integrity of game files' restores the stock DLL;"; \
	echo ">>       just re-run 'make install' if that happens."

# Put the stock DLL back and remove the mod entirely.
uninstall:
	@set -e; \
	if [ -z "$(GAME_DIR)" ]; then echo "ERROR: GAME_DIR is not set."; exit 1; fi; \
	orig="$(GAME_DIR)/LTMemory_orig.dll"; \
	if [ ! -f "$$orig" ]; then echo "Nothing to do: $$orig not present."; exit 0; fi; \
	for stray in $$(ls "$(GAME_DIR)" | grep -ix 'ltmemory\.dll'); do \
	  if grep -aq "Fear2AwardUnlocker" "$(GAME_DIR)/$$stray" 2>/dev/null; then \
	    rm -f "$(GAME_DIR)/$$stray"; \
	  fi; \
	done; \
	mv -f "$$orig" "$(GAME_DIR)/LTMemory.dll"; \
	rm -f "$(GAME_DIR)/Fear2AwardUnlocker.log"; \
	echo ">> restored stock LTMemory.dll and removed the mod"

# --- Distributable ----------------------------------------------------------
# `make dist` assembles dist/Fear2AwardUnlocker/ - the mod DLL plus the docs and
# an INSTALL.txt. It deliberately does NOT contain LTMemory_orig.dll: that is the
# user's own stock file, which they rename themselves (or `make install` does).
DIST    ?= dist
PAYLOAD  = $(DIST)/$(PROJECT)

dist: $(TARGET)
	@rm -rf "$(PAYLOAD)" && mkdir -p "$(PAYLOAD)"
	@cp $(TARGET) "$(PAYLOAD)/LTMemory.dll"
	@cp README.md LICENSE "$(PAYLOAD)/"
	@printf '%s\n' \
	  "$(PROJECT) $(VERSION) - in-game award toggles for F.E.A.R. 2: Project Origin" \
	  "" \
	  "INSTALL" \
	  "  1. Open your F.E.A.R. 2 folder (the one containing FEAR2.exe)." \
	  "  2. Rename the existing  ltmemory.dll  to  LTMemory_orig.dll" \
	  "  3. Copy  LTMemory.dll  from this folder in beside it." \
	  "" \
	  "  That is all - on Windows and on Linux/Proton alike. No launch options." \
	  "" \
	  "UNINSTALL" \
	  "  Delete LTMemory.dll and rename LTMemory_orig.dll back to ltmemory.dll." \
	  "" \
	  "USE" \
	  "  Press F8 in game to open the award panel." \
	  "" \
	  "NOTE" \
	  "  Toggling an award OFF zeroes the counter behind it, which can also" \
	  "  re-lock the matching mission in Mission Select. A one-time backup is" \
	  "  written to Profile000.gps.bak before the first change." \
	  > "$(PAYLOAD)/INSTALL.txt"
	@echo ">> assembled $(PAYLOAD)"

# <project>_<version>.zip, e.g. Fear2AwardUnlocker_1.0.0.zip
PKG ?= $(DIST)/$(PROJECT)_$(VERSION).zip
package: dist
	@command -v zip >/dev/null || { echo "ERROR: zip required"; exit 1; }
	@rm -f "$(PKG)"
	@cd "$(PAYLOAD)" && zip -r -q "$(abspath $(PKG))" .
	@echo ">> packaged -> $(PKG) ($$(du -h "$(PKG)" | cut -f1))"
