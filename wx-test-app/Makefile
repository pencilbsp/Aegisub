WX_CONFIG ?= ../build/subprojects/wxWidgets/__CMake_build/wx-config
WX_MONO ?= ../build/subprojects/wxWidgets/__CMake_build/lib/libwx_osx_cocoau-3.2.0.4.3.dylib
WX_RPATH := $(abspath ../build/subprojects/wxWidgets)
CXX ?= clang++
APP := build/wx-combo-alignment-test.app
BIN := $(APP)/Contents/MacOS/wx-combo-alignment-test

CXXFLAGS += -std=c++20 $(shell $(WX_CONFIG) --cxxflags)
LDFLAGS += $(WX_MONO) -Wl,-rpath,$(abspath ../build/subprojects/wxWidgets/__CMake_build/lib) -framework AppKit -framework Carbon -framework Cocoa -framework IOKit -framework QuartzCore -framework AudioToolbox -framework OpenGL

.PHONY: all clean

all: $(BIN)

$(BIN): main.mm Info.plist Makefile
	mkdir -p $(APP)/Contents/MacOS
	cp Info.plist $(APP)/Contents/Info.plist
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -rf build
