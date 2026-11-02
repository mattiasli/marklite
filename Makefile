# MarkLite - lightweight native Markdown editor for Windows
# Toolchain: MSYS2 UCRT64 GCC (mingw-w64).  Build:  mingw32-make  (or run build.bat)

CXX      = g++
CC       = gcc
WINDRES  = windres

BUILD    = build
SCI      = third_party/scintilla
MD4C     = third_party/md4c
TARGET   = MarkLite.exe

OPT      = -O2 -ffunction-sections -fdata-sections
DEFS     = -DNDEBUG -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00

APPFLAGS = -std=c++17 $(OPT) $(DEFS) -Wall -Wno-unknown-pragmas -I$(SCI)/include -I$(MD4C)
SCIFLAGS = -std=c++17 $(OPT) -DNDEBUG -DNO_CXX11_REGEX -I$(SCI)/include -I$(SCI)/src
CFLAGS   = $(OPT) -DNDEBUG -DMD4C_USE_UTF8

LDFLAGS  = -static -municode -mwindows -s -Wl,--gc-sections
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

.PHONY: all clean scintilla

all: $(TARGET)

scintilla: $(SCI_LIB)

$(TARGET): $(APP_OBJ) $(MD4C_OBJ) $(RES_OBJ) $(SCI_LIB)
	$(CXX) $(LDFLAGS) -o $@ $(APP_OBJ) $(MD4C_OBJ) $(RES_OBJ) $(SCI_LIB) $(LIBS)

$(SCI_LIB): $(SCI_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/sci/%.o: $(SCI)/%.cxx
	@mkdir -p $(dir $@)
	$(CXX) $(SCIFLAGS) -c $< -o $@

$(BUILD)/app/%.o: src/%.cpp $(APP_HDR)
	@mkdir -p $(dir $@)
	$(CXX) $(APPFLAGS) -c $< -o $@

$(MD4C_OBJ): $(MD4C)/md4c.c $(MD4C)/md4c.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(RES_OBJ): res/app.rc res/app.manifest res/app.ico src/resource.h
	@mkdir -p $(dir $@)
	$(WINDRES) -I src -O coff res/app.rc -o $@

clean:
	rm -rf $(BUILD) $(TARGET)
