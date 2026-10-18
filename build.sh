#!/usr/bin/env bash
# ==============================================================================
# Build & Test wrapper script for Restic Total Commander WFX Plugin
# Loads the Portable MSVC x64 environment and invokes NMake.
#
# Usage:
#   ./build.sh              # Build Debug plugin (default)
#   ./build.sh debug        # Build Debug plugin (/MTd)
#   ./build.sh release      # Build Release plugin (/MT, optimized)
#   ./build.sh test         # Build and run unit tests (Debug + CRT Leak Check)
#   ./build.sh asan         # Build and run unit tests with AddressSanitizer
#   ./build.sh analyze      # Run MSVC static code analysis (/analyze)
#   ./build.sh clean        # Clean build directory
# ==============================================================================
set -euo pipefail

MSVC_SETUP="D:\\_progetti\\_tool_\\msvc\\msvc\\setup_x64.bat"
ACTION="${1:-debug}"

run_nmake() {
    cmd.exe //c "${MSVC_SETUP} && nmake /nologo $*"
}

case "${ACTION}" in
    debug)
        run_nmake CONFIG=Debug all
        ;;
    release)
        run_nmake CONFIG=Release all
        ;;
    test)
        run_nmake CONFIG=Debug test
        ;;
    asan)
        run_nmake clean
        run_nmake CONFIG=Debug ASAN=1 test
        ;;
    analyze)
        run_nmake clean
        run_nmake CONFIG=Debug ANALYZE=1 all test
        ;;
    clean)
        run_nmake clean
        ;;
    *)
        echo "Usage: $0 {debug|release|test|asan|analyze|clean}" >&2
        exit 1
        ;;
esac
