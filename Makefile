OUT      = gw2app.dll
BUILD    = build

# Guild Wars 2 under Sikarugir (Wine wrapper for macOS).
# The prefix is inside the .app bundle; drive_c is a symlink to SharedSupport/prefix/drive_c.
GW2_APP    = /Applications/Guild Wars 2.app
GW2_DIR    = $(GW2_APP)/Contents/SharedSupport/prefix/drive_c/Program Files/Guild Wars 2
ADDONS_DIR = $(GW2_DIR)/addons
NEXUS_LOG  = $(ADDONS_DIR)/Nexus/Nexus.log

# Cross-compiler: we build a Windows x64 DLL from macOS.
# See docs/development.md for why this is a test-only artifact and CI builds the shippable one.
CXX     = x86_64-w64-mingw32-g++
WINDRES = x86_64-w64-mingw32-windres

SRCS = $(wildcard src/*.cpp) \
       $(wildcard src/UI/*.cpp) \
       $(wildcard src/Util/*.cpp) \
       vendor/imgui/imgui.cpp \
       vendor/imgui/imgui_draw.cpp \
       vendor/imgui/imgui_tables.cpp \
       vendor/imgui/imgui_widgets.cpp

OBJS = $(patsubst %.cpp,$(BUILD)/%.o,$(SRCS))
RES  = $(BUILD)/gw2app_res.o
DLL  = $(BUILD)/$(OUT)

CXXFLAGS = -std=c++20 -O2 -Wall -Wextra \
           -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
           -Isrc -Ires -Ivendor -Ivendor/imgui

# Static libgcc/libstdc++ so the DLL has no runtime dependencies to ship alongside it.
# ws2_32: the localhost server. shell32: opening URLs in the browser.
LDFLAGS = -shared -static -static-libgcc -static-libstdc++ -lws2_32 -lshell32

.PHONY: all
all: build

.PHONY: toolchain
toolchain:
	brew install mingw-w64

.PHONY: check
check:
	@command -v $(CXX) >/dev/null 2>&1 \
		&& echo "OK   compiler: $$($(CXX) --version | head -1)" \
		|| { echo "FAIL compiler not found. Run 'make toolchain'"; exit 1; }
	@test -d "$(GW2_DIR)" \
		&& echo "OK   GW2:      $(GW2_DIR)" \
		|| { echo "FAIL GW2 not found at $(GW2_DIR)"; exit 1; }
	@test -f "$(GW2_DIR)/d3d11.dll" \
		&& echo "OK   Nexus:    installed as d3d11.dll" \
		|| echo "WARN Nexus not detected (expected d3d11.dll in the GW2 folder)"
	@test -d "$(ADDONS_DIR)" \
		&& echo "OK   addons:   $(ADDONS_DIR)" \
		|| echo "WARN addons dir missing, 'make deploy' will create it"

.PHONY: build
build: $(DLL)

# Syntax-check one translation unit without needing the rest of the tree to link.
#   make tu FILE=src/Server.cpp
.PHONY: tu
tu:
	@test -n "$(FILE)" || { echo "usage: make tu FILE=src/Server.cpp"; exit 1; }
	$(CXX) $(CXXFLAGS) -fsyntax-only $(FILE) && echo "OK  $(FILE)"

$(DLL): $(OBJS) $(RES)
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $(OBJS) $(RES) $(LDFLAGS)
	@echo "built $@"

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Vendored third-party sources compile silently. ImGui 1.80 predates C++20's deprecation of
# bitwise ops between different enum types, and we cannot fix it: the version is pinned to match
# what Nexus links, so patching it is worse than the warning. Our own code keeps -Wall.
$(BUILD)/vendor/%.o: CXXFLAGS += -w

$(RES): res/gw2app.rc res/resource.h res/gw2app-icon.png res/gw2app-logo.png
	@mkdir -p $(dir $@)
	$(WINDRES) -I res $< -O coff -o $@

# GW2 holds the DLL open while running. Quit the game first, or this silently keeps the old one.
.PHONY: deploy
deploy: build
	@mkdir -p "$(ADDONS_DIR)"
	cp "$(DLL)" "$(ADDONS_DIR)/$(OUT)"
	@echo "deployed to $(ADDONS_DIR)/$(OUT)"

.PHONY: undeploy
undeploy:
	rm -f "$(ADDONS_DIR)/$(OUT)"
	@echo "removed $(ADDONS_DIR)/$(OUT)"

.PHONY: run
run:
	open -a "$(GW2_APP)"

.PHONY: logs
logs:
	@test -f "$(NEXUS_LOG)" || { echo "no log yet at $(NEXUS_LOG). Run the game once"; exit 1; }
	tail -f "$(NEXUS_LOG)"

.PHONY: log
log:
	@test -f "$(NEXUS_LOG)" || { echo "no log yet at $(NEXUS_LOG). Run the game once"; exit 1; }
	@grep -iE "gw2\.app|error|critical|warn" "$(NEXUS_LOG)" | tail -40

# Full loop: compile, install into the prefix, launch the game.
.PHONY: dev
dev: deploy run

.PHONY: clean
clean:
	rm -rf $(BUILD)
