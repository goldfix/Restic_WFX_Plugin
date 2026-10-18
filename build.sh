#!/usr/bin/env bash
# ==============================================================================
# Build & Test wrapper script for Restic Total Commander WFX Plugin
# Requires an active MSVC (x64) environment (cl.exe, link.exe, nmake.exe).
#
# Usage:
#   ./build.sh              # Build Debug plugin (default)
#   ./build.sh debug        # Build Debug plugin (/MTd)
#   ./build.sh release      # Build Release plugin (/MT, optimized)
#   ./build.sh test         # Build and run unit tests (Debug + CRT Leak Check)
#   ./build.sh release-test # Build and run unit tests against the optimized /MT build
#   ./build.sh asan         # Build and run unit tests with AddressSanitizer
#   ./build.sh analyze      # Run MSVC static code analysis (/analyze)
#   ./build.sh package      # Build Release, run tests, and create Total Commander installation ZIP
#   ./build.sh clean        # Clean build directory
# ==============================================================================
set -euo pipefail

ACTION="${1:-debug}"

# Under MSYS2 / Git Bash, convert POSIX-style drive prefixes (/c/..., /d/...)
# in INCLUDE and LIB to Windows drive prefixes (C:/..., D:/...) required by cl.exe/link.exe.
normalize_win_env_list() {
    local val="${1:-}"
    echo "${val}" | sed -E 's (^|;)/([a-zA-Z])/ \1\U\2:/ g'
}

if [[ -n "${INCLUDE:-}" ]]; then
    export INCLUDE="$(normalize_win_env_list "${INCLUDE}")"
fi
if [[ -n "${LIB:-}" ]]; then
    export LIB="$(normalize_win_env_list "${LIB}")"
fi

run_nmake() {
    # If Total Commander currently holds build/restic.wfx64 loaded in memory,
    # Windows forbids overwriting it in place but allows renaming it out of the way.
    if [[ -f "build/restic.wfx64" ]] && ! rm -f "build/restic.wfx64" 2>/dev/null; then
        mv -f "build/restic.wfx64" "build/restic.wfx64.old" 2>/dev/null || true
    fi
    MSYS2_ARG_CONV_EXCL="*" nmake -nologo "$@"
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
    release-test)
        run_nmake CONFIG=Release test
        ;;
    asan)
        run_nmake clean
        run_nmake CONFIG=Debug ASAN=1 test
        ;;
    analyze)
        run_nmake clean
        run_nmake CONFIG=Debug ANALYZE=1 all test
        ;;
    package)
        run_nmake clean
        run_nmake CONFIG=Release test
        run_nmake CONFIG=Release package
        ;;
    clean)
        run_nmake clean
        ;;
    *)
        echo "Usage: $0 {debug|release|test|release-test|asan|analyze|package|clean}" >&2
        exit 1
        ;;
esac
