# ==============================================================================
# NMake Makefile for Restic Total Commander WFX Plugin (x64)
# Usage (with MSVC x64 environment configured, or via ./build.sh):
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
TEST_CONFIG_OUT = $(BUILD_DIR)\test_config.exe
TEST_CLI_OUT    = $(BUILD_DIR)\test_restic_cli.exe
TEST_DLL_OUT    = $(BUILD_DIR)\test_plugin_dll.exe
DEF_FILE        = restic_wfx.def
INI_TEMPLATE    = restic_plugin.ini
INI_OUT         = $(BUILD_DIR)\restic_plugin.ini

CC   = cl.exe
LINK = link.exe

# Strict pure C11 (/TC /std:c11) compiler flags and security hardening (/GS)
CFLAGS_COMMON = /nologo /TC /std:c11 /permissive- /W4 /WX /we4013 /GS \
                /DUNICODE /D_UNICODE /DCJSON_HIDE_SYMBOLS /Iinclude /IcJSON \
                /Fo$(BUILD_DIR)\ /Fd$(BUILD_DIR)\vc.pdb

# Linker security hardening (/DYNAMICBASE for ASLR, /NXCOMPAT for DEP)
LFLAGS_COMMON = /NOLOGO /DEBUG /DYNAMICBASE /NXCOMPAT
SYSLIBS       =
TEST_LIBS     = Shell32.lib

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

PLUGIN_OBJS = $(BUILD_DIR)\main.obj $(BUILD_DIR)\config.obj $(BUILD_DIR)\restic_cli.obj $(BUILD_DIR)\cJSON.obj

TEST_CONFIG_OBJS = $(BUILD_DIR)\test_config.obj $(PLUGIN_OBJS)
TEST_CLI_OBJS    = $(BUILD_DIR)\test_restic_cli.obj $(PLUGIN_OBJS)

all: $(BUILD_DIR) $(PLUGIN_OUT) $(INI_OUT)

$(BUILD_DIR):
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"

$(INI_OUT): $(BUILD_DIR) $(INI_TEMPLATE)
	copy /Y "$(INI_TEMPLATE)" "$(INI_OUT)" >nul

$(BUILD_DIR)\main.obj: $(BUILD_DIR) src\main.c include\fsplugin.h include\config.h
	$(CC) /c $(CFLAGS) src\main.c

$(BUILD_DIR)\config.obj: $(BUILD_DIR) src\config.c include\config.h
	$(CC) /c $(CFLAGS) src\config.c

$(BUILD_DIR)\restic_cli.obj: $(BUILD_DIR) src\restic_cli.c include\restic_cli.h include\fsplugin.h cJSON\cJSON.h
	$(CC) /c $(CFLAGS) src\restic_cli.c

$(BUILD_DIR)\cJSON.obj: $(BUILD_DIR) cJSON\cJSON.c cJSON\cJSON.h
	$(CC) /c /nologo /TC /std:c11 /W3 /GS /DCJSON_HIDE_SYMBOLS $(CFLAGS_CFG) $(CFLAGS_SAN) /IcJSON /Fo$(BUILD_DIR)\ /Fd$(BUILD_DIR)\vc.pdb cJSON\cJSON.c

$(BUILD_DIR)\test_config.obj: $(BUILD_DIR) tests\test_config.c include\config.h include\fsplugin.h
	$(CC) /c $(CFLAGS) tests\test_config.c

$(BUILD_DIR)\test_restic_cli.obj: $(BUILD_DIR) tests\test_restic_cli.c include\restic_cli.h include\fsplugin.h
	$(CC) /c $(CFLAGS) tests\test_restic_cli.c

$(PLUGIN_OUT): $(PLUGIN_OBJS) $(DEF_FILE)
	$(CC) /nologo /LD $(CFLAGS_CFG) $(CFLAGS_SAN) $(PLUGIN_OBJS) $(SYSLIBS) /link $(LFLAGS_CFG) /DEF:$(DEF_FILE) /IMPLIB:$(PLUGIN_LIB) /OUT:$(PLUGIN_OUT)

$(TEST_CONFIG_OUT): $(TEST_CONFIG_OBJS)
	$(CC) /nologo $(CFLAGS_CFG) $(CFLAGS_SAN) $(TEST_CONFIG_OBJS) $(SYSLIBS) /link $(LFLAGS_CFG) /OUT:$(TEST_CONFIG_OUT)

$(TEST_CLI_OUT): $(TEST_CLI_OBJS)
	$(CC) /nologo $(CFLAGS_CFG) $(CFLAGS_SAN) $(TEST_CLI_OBJS) $(SYSLIBS) $(TEST_LIBS) /link $(LFLAGS_CFG) /OUT:$(TEST_CLI_OUT)

$(BUILD_DIR)\test_plugin_dll.obj: $(BUILD_DIR) tests\test_plugin_dll.c
	$(CC) /c $(CFLAGS) tests\test_plugin_dll.c

# Black-box smoke test: loads build\restic.wfx64 like Total Commander does (links no plugin code)
$(TEST_DLL_OUT): $(BUILD_DIR)\test_plugin_dll.obj
	$(CC) /nologo $(CFLAGS_CFG) $(CFLAGS_SAN) $(BUILD_DIR)\test_plugin_dll.obj /link $(LFLAGS_CFG) /OUT:$(TEST_DLL_OUT)

# The DLL smoke test needs the plugin binary; AddressSanitizer runs only exercise the unit tests
TEST_DEPS = $(INI_OUT) $(TEST_CONFIG_OUT) $(TEST_CLI_OUT) $(TEST_DLL_OUT)
!IF "$(ASAN)" != "1"
TEST_DEPS = $(PLUGIN_OUT) $(TEST_DEPS)
!ENDIF

test: $(TEST_DEPS)
	$(TEST_CONFIG_OUT)
	$(TEST_CLI_OUT)
	$(TEST_DLL_OUT)

analyze:
	$(MAKE) /nologo ANALYZE=1 $(PLUGIN_OUT) $(INI_OUT) $(TEST_CONFIG_OUT) $(TEST_CLI_OUT) $(TEST_DLL_OUT)

clean:
	@if exist "$(BUILD_DIR)" rmdir /s /q "$(BUILD_DIR)"
