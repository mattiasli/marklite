# MarkLite - lightweight native Markdown editor for Windows
# Toolchain: MSYS2 UCRT64 GCC (mingw-w64). Run from the repository root:
#
#   mingw32-make              MarkLite.exe
#   mingw32-make test         build and run the unit tests
#   mingw32-make clean        remove all build output
#
# build.bat runs the same targets from a normal Command Prompt.

CXX      = g++
CC       = gcc
WINDRES  = windres

BUILD    = build
SCI      = third_party/scintilla
MD4C     = third_party/md4c
TARGET   = MarkLite.exe
SETUP    = MarkLite-Setup.exe

# The version lives in one place, the VERSION file. It is compiled into the app, the installer
# and the version resources through the generated header build/version.h.
VERSION     := $(strip $(file <VERSION))
comma       := ,
VERSION_NUM := $(subst .,$(comma),$(VERSION))$(comma)0
VERSION_H   := $(BUILD)/version.h

OPT      = -O2 -ffunction-sections -fdata-sections
DEFS     = -DNDEBUG -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00

APPFLAGS = -std=c++17 $(OPT) $(DEFS) -Wall -Wno-unknown-pragmas -I$(SCI)/include -I$(MD4C) -I$(BUILD)
SCIFLAGS = -std=c++17 $(OPT) -DNDEBUG -DNO_CXX11_REGEX -I$(SCI)/include -I$(SCI)/src
CFLAGS   = $(OPT) -DNDEBUG -DMD4C_USE_UTF8

LDFLAGS  = -static -municode -mwindows -s -Wl,--gc-sections -Wl,--dynamicbase,--nxcompat,--high-entropy-va
LIBS     = -ld2d1 -ldwrite -lwindowscodecs -lwinhttp -lcrypt32 -lole32 -loleaut32 -luuid \
           -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi -luxtheme -ldwmapi -limm32 -lmsimg32 \
           -lgdi32 -luser32 -ladvapi32

SCI_SRC  = $(wildcard $(SCI)/src/*.cxx) \
           $(SCI)/win32/HanjaDic.cxx $(SCI)/win32/PlatWin.cxx $(SCI)/win32/ListBox.cxx \
           $(SCI)/win32/SurfaceGDI.cxx $(SCI)/win32/SurfaceD2D.cxx $(SCI)/win32/ScintillaWin.cxx
SCI_OBJ  = $(patsubst $(SCI)/%.cxx,$(BUILD)/sci/%.o,$(SCI_SRC))
SCI_LIB  = $(BUILD)/libscintilla.a

APP_SRC  = $(wildcard src/*.cpp)
APP_OBJ  = $(patsubst src/%.cpp,$(BUILD)/app/%.o,$(APP_SRC))
APP_HDR  = $(wildcard src/*.h)
MD4C_OBJ = $(BUILD)/md4c.o

# UI code that only runs on user actions is optimised for size; the hot paths (editor styling,
# preview layout/rendering, spell checking, Markdown parsing, Scintilla) stay at -O2.
SIZE_OPT = aipanel ai clipimage dialogs findbar format json settings tabs theme toolbar
$(patsubst %,$(BUILD)/app/%.o,$(SIZE_OPT)): OPT = -Os -ffunction-sections -fdata-sections
RES_OBJ  = $(BUILD)/app.res.o

# ---------------------------------------------------------------- unit tests
# The tests link the app's formatting, editor styling, theme, settings and JSON code and drive a real
# (hidden) Scintilla control. tests/stubs.cpp stands in for the few main window services they use.
TEST_SRC     = $(wildcard tests/*.cpp)
TEST_OBJ     = $(patsubst tests/%.cpp,$(BUILD)/tests/%.o,$(TEST_SRC))
TEST_APP_OBJ = $(patsubst %,$(BUILD)/app/%.o,format editor theme settings json)
TESTS        = $(BUILD)/tests.exe

.PHONY: all clean scintilla tests test

all: $(TARGET)

tests: $(TESTS)

test: $(TESTS)
	./$(TESTS)

scintilla: $(SCI_LIB)

$(BUILD):
	@mkdir -p $@

$(VERSION_H): VERSION | $(BUILD)
	$(if $(VERSION),,$(error VERSION file is missing or empty))
	$(file >$@,// Generated from VERSION by the Makefile. Do not edit.)
	$(file >>$@,#pragma once)
	$(file >>$@,#define MARKLITE_VERSION "$(VERSION)")
	$(file >>$@,#define MARKLITE_VERSION_W L"$(VERSION)")
	$(file >>$@,#define MARKLITE_VERSION_NUM $(VERSION_NUM))
	@echo "version $(VERSION)"

$(TARGET): $(APP_OBJ) $(MD4C_OBJ) $(RES_OBJ) $(SCI_LIB)
	$(CXX) $(LDFLAGS) -o $@ $(APP_OBJ) $(MD4C_OBJ) $(RES_OBJ) $(SCI_LIB) $(LIBS)

$(SCI_LIB): $(SCI_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/sci/%.o: $(SCI)/%.cxx
	@mkdir -p $(dir $@)
	$(CXX) $(SCIFLAGS) -c $< -o $@

$(BUILD)/app/%.o: src/%.cpp $(APP_HDR) $(VERSION_H)
	@mkdir -p $(dir $@)
	$(CXX) $(APPFLAGS) -c $< -o $@

$(MD4C_OBJ): $(MD4C)/md4c.c $(MD4C)/md4c.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(RES_OBJ): res/app.rc res/app.manifest res/app.ico src/resource.h $(VERSION_H)
	@mkdir -p $(dir $@)
	$(WINDRES) -I src -I $(BUILD) -O coff res/app.rc -o $@

# ---------------------------------------------------------------- test rules
$(BUILD)/tests/%.o: tests/%.cpp $(wildcard tests/*.h) $(APP_HDR) $(VERSION_H)
	@mkdir -p $(dir $@)
	$(CXX) $(APPFLAGS) -Isrc -c $< -o $@

$(TESTS): $(TEST_OBJ) $(TEST_APP_OBJ) $(SCI_LIB)
	$(CXX) -static -municode -mconsole -o $@ $(TEST_OBJ) $(TEST_APP_OBJ) $(SCI_LIB) $(LIBS)

clean:
	rm -rf $(BUILD) $(TARGET)
