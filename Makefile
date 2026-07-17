# hktuner — chromatic tuner LV2 plugin for Haiku.
#
# Builds against the sibling LV2-haiku checkout's staged headers (pkg-config
# against build/stage; a plugin needs only the lv2 headers — no lilv). The
# development loop runs entirely on the Haiku laptop:
#
#   make remote        # rsync to haikulaptop, build over ssh, install
#
# Never treat a Linux build as authoritative; the target is Haiku.

LV2_HAIKU ?= $(HOME)/LV2-haiku
STAGE := $(LV2_HAIKU)/build/stage
PKGCFG := PKG_CONFIG_PATH=$(STAGE)/lib/pkgconfig

BUILD_DIR ?= build
OUT := $(BUILD_DIR)/hktuner.lv2
LV2_INSTALL_DIR ?= $(HOME)/config/non-packaged/add-ons/lv2

CC ?= gcc
CXX ?= g++
WARN = -Wall -Wextra
UNAME := $(shell uname)

GUI_PNGS := $(wildcard gui/*.png)
BUNDLE_PNGS := $(patsubst gui/%.png,$(OUT)/gui/%.png,$(GUI_PNGS))
TTLS := $(OUT)/manifest.ttl $(OUT)/hktuner.ttl

all: build

ifeq ($(UNAME),Haiku)
ifneq ($(wildcard src/hktuner_ui.cpp),)
# The native BView UI needs the Interface Kit (Haiku only).
build: check-stage $(OUT)/hktuner.so $(OUT)/hktuner_ui.so $(TTLS) $(BUNDLE_PNGS)
else
# TODO: the UI translation unit does not exist yet; until it lands the bundle
# ships DSP-only (hosts that try the declared UI will report a load failure).
build: check-stage $(OUT)/hktuner.so $(TTLS) $(BUNDLE_PNGS)
endif
else
build: check-stage $(OUT)/hktuner.so $(TTLS) $(BUNDLE_PNGS)
endif

# The stage must exist before anything compiles; fail with a clear message
# instead of a cryptic pkg-config one.
check-stage:
	@test -f $(STAGE)/lib/pkgconfig/lv2.pc || { \
		echo "error: $(STAGE)/lib/pkgconfig/lv2.pc not found;" \
		     "build the LV2-haiku stack first (make in $(LV2_HAIKU))" >&2; \
		exit 1; }

$(OUT)/hktuner.so: src/hktuner.c
	@mkdir -p $(OUT)
	$(CC) -std=c99 $(WARN) -O2 -fPIC -shared -o $@ $< \
		`$(PKGCFG) pkg-config --cflags lv2` -lm

# Haiku only: the plugin's native BView UI (second .so in the bundle).
# Translation Kit (PNG loading) lives in libtranslation.
$(OUT)/hktuner_ui.so: src/hktuner_ui.cpp
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 $(WARN) -O2 -fPIC -shared -o $@ $< \
		`$(PKGCFG) pkg-config --cflags lv2` -lbe -ltranslation

$(OUT)/%.ttl: src/%.ttl
	@mkdir -p $(OUT)
	cp $< $@

$(OUT)/gui/%.png: gui/%.png
	@mkdir -p $(OUT)/gui
	cp $< $@

# ---- install / validate ----------------------------------------------------

install: build
	mkdir -p $(LV2_INSTALL_DIR)
	rm -rf $(LV2_INSTALL_DIR)/hktuner.lv2
	cp -r $(OUT) $(LV2_INSTALL_DIR)/hktuner.lv2

validate:
	@echo "== lv2ls =="
	$(STAGE)/bin/lv2ls
	@echo "== lv2info hktuner =="
	$(STAGE)/bin/lv2info https://github.com/rations/hktuner

# ---- art (dev host, one-shot; outputs are committed) -----------------------

art:
	gui/make_background.sh
	gui/make_led_cells.sh
	gui/make_dots.sh
	gui/make_glyphs.sh

# ---- dev-host loop ---------------------------------------------------------

sync:
	rsync -a --delete --exclude build --exclude .git ./ haikulaptop:hktuner/

remote: sync
	ssh haikulaptop 'cd hktuner && make && make install'

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all build check-stage install validate art sync remote clean
