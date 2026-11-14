# ==============================================================================
# NMake Makefile for Restic Total Commander WFX Plugin (x64 and x86)
#
# ARCH selects the target architecture (the compiler/linker must be initialized
# for it, ./build.sh switches the environment for you):
#   ARCH=x64 (default) -> build\x64\restic.wfx64
#   ARCH=x86           -> build\x86\restic.wfx
#
# Usage (with an initialized MSVC environment, or via ./build.sh):
#   nmake /nologo                     -> Build Debug plugin
#   nmake /nologo CONFIG=Release      -> Build Release plugin (static CRT /MT)
#   nmake /nologo ARCH=x86            -> Build the 32-bit Debug plugin
#   nmake /nologo test                -> Build and run unit tests
#   nmake /nologo test ASAN=1         -> Build and run unit tests with AddressSanitizer
#   nmake /nologo analyze             -> Run MSVC static analysis (/analyze)
#   nmake /nologo package             -> Build Release plugin and stage the install files
#   nmake /nologo zip                 -> Create the two install ZIPs (x86, x64)
#   nmake /nologo clean               -> Remove the artifacts of ARCH only
#   nmake /nologo cleanall            -> Remove the whole build tree
# ==============================================================================

!IF "$(CONFIG)" == ""
CONFIG = Debug
!ENDIF

!IF "$(ARCH)" == ""
ARCH = x64
!ENDIF

# Total Commander loads <name>.wfx from 32-bit and <name>.wfx64 from 64-bit,
# both files sitting in the same folder (WFX SDK, "64-bit Support").
!IF "$(ARCH)" == "x86"
ARCH_DIR     = x86
PLUGIN_NAME  = restic.wfx
DEF_FILE     = restic_wfx32.def
CFLAGS_ARCH  =
RCFLAGS_ARCH = /DRC_ARCH_X86
LFLAGS_ARCH  = /MACHINE:X86 /SAFESEH
!ELSEIF "$(ARCH)" == "x64"
ARCH_DIR     = x64
PLUGIN_NAME  = restic.wfx64
DEF_FILE     = restic_wfx.def
CFLAGS_ARCH  =
RCFLAGS_ARCH =
LFLAGS_ARCH  = /MACHINE:X64
!ELSE
!ERROR ARCH must be x64 or x86, not "$(ARCH)"
!ENDIF

BUILD_ROOT = build
BUILD_DIR  = $(BUILD_ROOT)\$(ARCH_DIR)
PLUGIN_OUT = $(BUILD_DIR)\$(PLUGIN_NAME)
PLUGIN_LIB = $(BUILD_DIR)\restic.lib
PLUGIN_RES = $(BUILD_DIR)\restic_wfx.res
TEST_CONFIG_OUT = $(BUILD_DIR)\test_config.exe
TEST_CLI_OUT    = $(BUILD_DIR)\test_restic_cli.exe
TEST_DLL_OUT    = $(BUILD_DIR)\test_plugin_dll.exe
TEST_STATS_OUT  = $(BUILD_DIR)\test_stats.exe
TEST_REPORT_OUT = $(BUILD_DIR)\test_report.exe
RC_FILE         = src\restic_wfx.rc
ICO_FILE        = logo\restic_wfx.ico
INI_TEMPLATE    = restic_plugin.ini
INI_OUT         = $(BUILD_DIR)\restic_plugin.ini
# One install manifest per architecture: it names the DLL that the package carries
PLUGINST_FILE   = pluginst_$(ARCH_DIR).inf
PLUGINST_OUT    = $(BUILD_DIR)\pluginst.inf

CC   = cl.exe
RC   = rc.exe

# Strict pure C11 (/TC /std:c11) compiler flags and security hardening (/GS)
CFLAGS_COMMON = /nologo /TC /std:c11 /permissive- /W4 /WX /we4013 /GS \
                /DUNICODE /D_UNICODE /DCJSON_HIDE_SYMBOLS /Iinclude /IcJSON \
                /Fo$(BUILD_DIR)\ /Fd$(BUILD_DIR)\vc.pdb

# Linker security hardening (/DYNAMICBASE for ASLR, /NXCOMPAT for DEP).
# /SAFESEH and /MACHINE are architecture specific (see LFLAGS_ARCH).
LFLAGS_COMMON = /NOLOGO /DEBUG /DYNAMICBASE /NXCOMPAT $(LFLAGS_ARCH)
SYSLIBS       = User32.lib
TEST_LIBS     = Shell32.lib

!IF "$(CONFIG)" == "Release"
CFLAGS_CFG = /MT /O2 /Zi /DNDEBUG
LFLAGS_CFG = $(LFLAGS_COMMON) /OPT:REF /OPT:ICF
RCFLAGS    = /nologo /Iinclude /DNDEBUG $(RCFLAGS_ARCH)
!ELSE
CFLAGS_CFG = /MTd /Zi /Od /D_DEBUG
LFLAGS_CFG = $(LFLAGS_COMMON)
RCFLAGS    = /nologo /Iinclude /D_DEBUG $(RCFLAGS_ARCH)
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

CFLAGS = $(CFLAGS_COMMON) $(CFLAGS_CFG) $(CFLAGS_EXTRA) $(CFLAGS_ARCH)

PLUGIN_OBJS = $(BUILD_DIR)\main.obj $(BUILD_DIR)\config.obj $(BUILD_DIR)\restic_cli.obj \
              $(BUILD_DIR)\restic_stats.obj $(BUILD_DIR)\wfx_path.obj $(BUILD_DIR)\fscontent.obj \
              $(BUILD_DIR)\restic_report.obj $(BUILD_DIR)\wfx_status.obj \
              $(BUILD_DIR)\wfx_trace.obj $(BUILD_DIR)\cJSON.obj

TEST_CONFIG_OBJS = $(BUILD_DIR)\test_config.obj $(PLUGIN_OBJS)
TEST_CLI_OBJS    = $(BUILD_DIR)\test_restic_cli.obj $(PLUGIN_OBJS)
TEST_STATS_OBJS  = $(BUILD_DIR)\test_stats.obj $(PLUGIN_OBJS)
TEST_REPORT_OBJS = $(BUILD_DIR)\test_report.obj $(PLUGIN_OBJS)

all: $(BUILD_DIR) $(PLUGIN_OUT) $(INI_OUT) $(PLUGINST_OUT)

$(BUILD_DIR):
	@if not exist "$(BUILD_ROOT)" mkdir "$(BUILD_ROOT)"
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"

$(INI_OUT): $(BUILD_DIR) $(INI_TEMPLATE)
	copy /Y "$(INI_TEMPLATE)" "$(INI_OUT)" >nul

$(PLUGINST_OUT): $(BUILD_DIR) $(PLUGINST_FILE)
	copy /Y "$(PLUGINST_FILE)" "$(PLUGINST_OUT)" >nul

$(BUILD_DIR)\main.obj: $(BUILD_DIR) src\main.c include\fsplugin.h include\config.h include\restic_cli.h include\restic_stats.h include\restic_report.h include\wfx_path.h include\wfx_status.h include\wfx_trace.h
	$(CC) /c $(CFLAGS) src\main.c

$(BUILD_DIR)\config.obj: $(BUILD_DIR) src\config.c include\config.h
	$(CC) /c $(CFLAGS) src\config.c

$(BUILD_DIR)\restic_cli.obj: $(BUILD_DIR) src\restic_cli.c include\restic_cli.h include\fsplugin.h cJSON\cJSON.h
	$(CC) /c $(CFLAGS) src\restic_cli.c

$(BUILD_DIR)\restic_stats.obj: $(BUILD_DIR) src\restic_stats.c include\restic_stats.h include\restic_cli.h cJSON\cJSON.h
	$(CC) /c $(CFLAGS) src\restic_stats.c

$(BUILD_DIR)\wfx_path.obj: $(BUILD_DIR) src\wfx_path.c include\wfx_path.h include\restic_cli.h
	$(CC) /c $(CFLAGS) src\wfx_path.c

$(BUILD_DIR)\restic_report.obj: $(BUILD_DIR) src\restic_report.c include\restic_report.h include\restic_cli.h include\restic_stats.h cJSON\cJSON.h
	$(CC) /c $(CFLAGS) src\restic_report.c

$(BUILD_DIR)\wfx_status.obj: $(BUILD_DIR) src\wfx_status.c include\wfx_status.h include\fsplugin.h include\wfx_trace.h
	$(CC) /c $(CFLAGS) src\wfx_status.c

$(BUILD_DIR)\fscontent.obj: $(BUILD_DIR) src\fscontent.c include\fscontent.h include\fsplugin.h include\restic_stats.h include\restic_report.h include\wfx_path.h include\wfx_trace.h
	$(CC) /c $(CFLAGS) src\fscontent.c

$(BUILD_DIR)\wfx_trace.obj: $(BUILD_DIR) src\wfx_trace.c include\wfx_trace.h
	$(CC) /c $(CFLAGS) src\wfx_trace.c

$(BUILD_DIR)\cJSON.obj: $(BUILD_DIR) cJSON\cJSON.c cJSON\cJSON.h
	$(CC) /c /nologo /TC /std:c11 /W3 /GS /DCJSON_HIDE_SYMBOLS $(CFLAGS_CFG) $(CFLAGS_SAN) $(CFLAGS_ARCH) /IcJSON /Fo$(BUILD_DIR)\ /Fd$(BUILD_DIR)\vc.pdb cJSON\cJSON.c

$(BUILD_DIR)\test_config.obj: $(BUILD_DIR) tests\test_config.c include\config.h include\fsplugin.h
	$(CC) /c $(CFLAGS) tests\test_config.c

$(BUILD_DIR)\test_restic_cli.obj: $(BUILD_DIR) tests\test_restic_cli.c include\restic_cli.h include\fsplugin.h
	$(CC) /c $(CFLAGS) tests\test_restic_cli.c

$(BUILD_DIR)\test_stats.obj: $(BUILD_DIR) tests\test_stats.c include\restic_stats.h include\fscontent.h include\wfx_path.h include\wfx_trace.h
	$(CC) /c $(CFLAGS) tests\test_stats.c

$(BUILD_DIR)\test_report.obj: $(BUILD_DIR) tests\test_report.c include\restic_report.h include\restic_cli.h include\restic_stats.h include\wfx_path.h include\wfx_status.h
	$(CC) /c $(CFLAGS) tests\test_report.c

$(PLUGIN_RES): $(BUILD_DIR) $(RC_FILE) $(ICO_FILE) include\version.h
	$(RC) $(RCFLAGS) /fo$(PLUGIN_RES) $(RC_FILE)

$(PLUGIN_OUT): $(PLUGIN_OBJS) $(PLUGIN_RES) $(DEF_FILE)
	$(CC) /nologo /LD $(CFLAGS_CFG) $(CFLAGS_SAN) $(PLUGIN_OBJS) $(PLUGIN_RES) $(SYSLIBS) /link $(LFLAGS_CFG) /DEF:$(DEF_FILE) /IMPLIB:$(PLUGIN_LIB) /OUT:$(PLUGIN_OUT)

$(TEST_CONFIG_OUT): $(TEST_CONFIG_OBJS)
	$(CC) /nologo $(CFLAGS_CFG) $(CFLAGS_SAN) $(TEST_CONFIG_OBJS) $(SYSLIBS) /link $(LFLAGS_CFG) /OUT:$(TEST_CONFIG_OUT)

$(TEST_CLI_OUT): $(TEST_CLI_OBJS)
	$(CC) /nologo $(CFLAGS_CFG) $(CFLAGS_SAN) $(TEST_CLI_OBJS) $(SYSLIBS) $(TEST_LIBS) /link $(LFLAGS_CFG) /OUT:$(TEST_CLI_OUT)

$(TEST_STATS_OUT): $(TEST_STATS_OBJS)
	$(CC) /nologo $(CFLAGS_CFG) $(CFLAGS_SAN) $(TEST_STATS_OBJS) $(SYSLIBS) /link $(LFLAGS_CFG) /OUT:$(TEST_STATS_OUT)

$(TEST_REPORT_OUT): $(TEST_REPORT_OBJS)
	$(CC) /nologo $(CFLAGS_CFG) $(CFLAGS_SAN) $(TEST_REPORT_OBJS) $(SYSLIBS) /link $(LFLAGS_CFG) /OUT:$(TEST_REPORT_OUT)

$(BUILD_DIR)\test_plugin_dll.obj: $(BUILD_DIR) tests\test_plugin_dll.c include\version.h
	$(CC) /c $(CFLAGS) tests\test_plugin_dll.c

# Black-box smoke test: loads the plugin DLL like Total Commander does (links no plugin code)
$(TEST_DLL_OUT): $(BUILD_DIR)\test_plugin_dll.obj
	$(CC) /nologo $(CFLAGS_CFG) $(CFLAGS_SAN) $(BUILD_DIR)\test_plugin_dll.obj /link $(LFLAGS_CFG) $(SYSLIBS) /OUT:$(TEST_DLL_OUT)

# The DLL smoke test needs the plugin binary; AddressSanitizer runs only exercise the unit tests
TEST_DEPS = $(INI_OUT) $(PLUGINST_OUT) $(TEST_CONFIG_OUT) $(TEST_CLI_OUT) $(TEST_STATS_OUT) $(TEST_REPORT_OUT) $(TEST_DLL_OUT)
!IF "$(ASAN)" != "1"
TEST_DEPS = $(PLUGIN_OUT) $(TEST_DEPS)
!ENDIF

test: $(TEST_DEPS)
	$(TEST_CONFIG_OUT)
	$(TEST_CLI_OUT)
	$(TEST_STATS_OUT)
	$(TEST_REPORT_OUT)
	$(TEST_DLL_OUT)

analyze:
	$(MAKE) /nologo ARCH=$(ARCH) ANALYZE=1 $(PLUGIN_OUT) $(INI_OUT) $(PLUGINST_OUT) $(TEST_CONFIG_OUT) $(TEST_CLI_OUT) $(TEST_STATS_OUT) $(TEST_REPORT_OUT) $(TEST_DLL_OUT)

# Release build of one architecture plus the files that go next to the DLL
package:
	$(MAKE) /nologo ARCH=$(ARCH) CONFIG=Release all

# Install packages: one ZIP per architecture (32-bit restic.wfx, 64-bit restic.wfx64)
zip:
	python tools\package.py

clean:
	@if exist "$(BUILD_DIR)" rmdir /s /q "$(BUILD_DIR)"

cleanall:
	@if exist "$(BUILD_ROOT)" rmdir /s /q "$(BUILD_ROOT)"
