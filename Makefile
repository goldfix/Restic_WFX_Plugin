# ==============================================================================
# NMake Makefile for Restic Total Commander WFX Plugin (x64)
# Usage (after running setup_x64.bat, or via ./build.sh):
#   nmake /nologo                  -> Build Debug plugin (build\restic.wfx64)
#   nmake /nologo CONFIG=Release   -> Build Release plugin (static CRT /MT)
#   nmake /nologo test             -> Build and run unit tests
#   nmake /nologo test ASAN=1      -> Build and run unit tests with AddressSanitizer
#   nmake /nologo analyze          -> Run MSVC static analysis (/analyze)
#   nmake /nologo clean            -> Remove build artifacts
# ==============================================================================

!IF "$(CONFIG)" == ""
CONFIG = Debug
!ENDIF

BUILD_DIR  = build
PLUGIN_OUT = $(BUILD_DIR)\restic.wfx64
PLUGIN_LIB = $(BUILD_DIR)\restic.lib
TEST_OUT   = $(BUILD_DIR)\test_config.exe
DEF_FILE   = restic_wfx.def

CC   = cl.exe
LINK = link.exe

# Strict pure C11 (/TC /std:c11) compiler flags and security hardening (/GS)
CFLAGS_COMMON = /nologo /TC /std:c11 /permissive- /W4 /WX /we4013 /GS \
                /DUNICODE /D_UNICODE /Iinclude /IcJSON \
                /Fo$(BUILD_DIR)\ /Fd$(BUILD_DIR)\vc.pdb

# Linker security hardening (/DYNAMICBASE for ASLR, /NXCOMPAT for DEP)
LFLAGS_COMMON = /NOLOGO /DEBUG /DYNAMICBASE /NXCOMPAT
SYSLIBS       = User32.lib Advapi32.lib Shlwapi.lib

!IF "$(CONFIG)" == "Release"
CFLAGS_CFG = /MT /O2 /Zi /DNDEBUG
LFLAGS_CFG = $(LFLAGS_COMMON) /OPT:REF /OPT:ICF
!ELSE
CFLAGS_CFG = /MTd /Zi /Od /D_DEBUG
LFLAGS_CFG = $(LFLAGS_COMMON)
!ENDIF

CFLAGS_EXTRA =
CFLAGS_SAN   =
!IF "$(ANALYZE)" == "1"
CFLAGS_EXTRA = $(CFLAGS_EXTRA) /analyze
!ENDIF

!IF "$(ASAN)" == "1"
CFLAGS_EXTRA = $(CFLAGS_EXTRA) /fsanitize=address
CFLAGS_SAN   = /fsanitize=address
LFLAGS_CFG   = $(LFLAGS_CFG) /INCREMENTAL:NO
!ENDIF

CFLAGS = $(CFLAGS_COMMON) $(CFLAGS_CFG) $(CFLAGS_EXTRA)

PLUGIN_OBJS = $(BUILD_DIR)\main.obj $(BUILD_DIR)\config.obj $(BUILD_DIR)\cJSON.obj

TEST_OBJS = $(BUILD_DIR)\test_config.obj $(PLUGIN_OBJS)

all: $(BUILD_DIR) $(PLUGIN_OUT)

$(BUILD_DIR):
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"

$(BUILD_DIR)\main.obj: $(BUILD_DIR) src\main.c include\fsplugin.h include\config.h
	$(CC) /c $(CFLAGS) src\main.c

$(BUILD_DIR)\config.obj: $(BUILD_DIR) src\config.c include\config.h
	$(CC) /c $(CFLAGS) src\config.c

$(BUILD_DIR)\cJSON.obj: $(BUILD_DIR) cJSON\cJSON.c cJSON\cJSON.h
	$(CC) /c /nologo /TC /std:c11 /W3 /GS $(CFLAGS_CFG) $(CFLAGS_SAN) /IcJSON /Fo$(BUILD_DIR)\ /Fd$(BUILD_DIR)\vc.pdb cJSON\cJSON.c

$(BUILD_DIR)\test_config.obj: $(BUILD_DIR) tests\test_config.c include\config.h include\fsplugin.h
	$(CC) /c $(CFLAGS) tests\test_config.c

$(PLUGIN_OUT): $(PLUGIN_OBJS) $(DEF_FILE)
	$(LINK) /DLL $(LFLAGS_CFG) /DEF:$(DEF_FILE) /IMPLIB:$(PLUGIN_LIB) /OUT:$(PLUGIN_OUT) $(PLUGIN_OBJS) $(SYSLIBS)

$(TEST_OUT): $(TEST_OBJS)
	$(LINK) $(LFLAGS_CFG) /OUT:$(TEST_OUT) $(TEST_OBJS) $(SYSLIBS)

test: $(TEST_OUT)
	$(TEST_OUT)

analyze:
	$(MAKE) /nologo ANALYZE=1 $(PLUGIN_OUT) $(TEST_OUT)

clean:
	@if exist "$(BUILD_DIR)" rmdir /s /q "$(BUILD_DIR)"
