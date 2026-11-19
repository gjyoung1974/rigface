# Rigface, C++ version.
#
#   make            build everything that can be built (see below)
#   make sim        build/rigface-sim   the simulated radio; no dependencies
#   make gui        build/rigface       the front panel; needs GTK 4 and GStreamer
#   make check      build and run the front panel's smoke test (needs a display)
#   make clean
#
# Packages needed for the front panel (Debian / Ubuntu / Raspberry Pi OS):
#   sudo apt install g++ make pkg-config libgtk-4-dev libgstreamer1.0-dev

CXX      ?= g++
CXXFLAGS ?= -O2 -g
WARN     := -Wall -Wextra
STD      := -std=c++20
BUILD    := build
PKGS     := gtk4 gstreamer-1.0

SIM_SRC := src/sim_main.cpp src/sim.cpp
GUI_SRC := $(filter-out $(SIM_SRC),$(wildcard src/*.cpp))
SIM_OBJ := $(SIM_SRC:src/%.cpp=$(BUILD)/%.o)
GUI_OBJ := $(GUI_SRC:src/%.cpp=$(BUILD)/%.o)

# Only the front panel needs GTK and GStreamer: the simulator builds without
# them, so their absence is an error only when the front panel is asked for.
ifneq ($(filter gui check $(BUILD)/rigface $(BUILD)/smoke_gui,$(MAKECMDGOALS))$(if $(MAKECMDGOALS),,$(wildcard src/app.cpp)),)
  MISSING := $(strip $(foreach p,$(PKGS),$(if $(shell pkg-config --exists $(p) && echo ok),,$(p))))
  ifneq ($(MISSING),)
    $(error Missing development packages for: $(MISSING). See the top of this Makefile)
  endif
endif
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs $(PKGS) 2>/dev/null)

# The front panel is only part of the default build once its sources exist.
ifneq ($(wildcard src/app.cpp),)
all: sim gui
else
all: sim
	@echo "Built the simulator only: src/app.cpp does not exist yet, so there is no front panel to build."
endif

sim: $(BUILD)/rigface-sim
gui: $(BUILD)/rigface $(BUILD)/rigface-sim      # --demo runs the simulator

$(BUILD)/rigface-sim: $(SIM_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ -pthread

$(BUILD)/rigface: $(GUI_OBJ)
	@if [ ! -f src/app.cpp ]; then \
	  echo "error: src/app.cpp is missing (app.h declares run_app but nothing defines it, and there is no main)"; \
	  exit 1; fi
	$(CXX) $(LDFLAGS) -o $@ $^ $(PKG_LIBS) -pthread -lm

# style.css is compiled into the program through this generated header.
$(BUILD)/style_css.h: src/style_css.h.in src/style.css | $(BUILD)
	awk 'NR == FNR { css = css $$0 "\n"; next } \
	     { i = index($$0, "@RIGFACE_STYLE_CSS@"); \
	       if (i) print substr($$0, 1, i - 1) css substr($$0, i + 19); else print }' \
	    src/style.css src/style_css.h.in > $@

$(GUI_OBJ): | $(BUILD)/style_css.h

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(STD) $(WARN) $(CXXFLAGS) $(CPPFLAGS) -I$(BUILD) -Isrc $(PKG_CFLAGS) -pthread -MMD -MP -c -o $@ $<

# The smoke test is the front panel with its own main().
$(BUILD)/smoke_gui: $(BUILD)/smoke_gui.o $(filter-out $(BUILD)/main.o,$(GUI_OBJ)) $(BUILD)/rigface-sim
	$(CXX) $(LDFLAGS) -o $@ $(filter %.o,$^) $(PKG_LIBS) -pthread -lm

$(BUILD)/smoke_gui.o: tests/smoke_gui.cpp | $(BUILD) $(BUILD)/style_css.h
	$(CXX) $(STD) $(WARN) $(CXXFLAGS) $(CPPFLAGS) -I$(BUILD) -Isrc $(PKG_CFLAGS) -pthread -MMD -MP -c -o $@ $<

check: $(BUILD)/smoke_gui
	$(BUILD)/smoke_gui

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all sim gui check clean
-include $(wildcard $(BUILD)/*.d)
