#!/usr/bin/env bash
# ==============================================================================
# Build & Test wrapper script for Restic Total Commander WFX Plugin
# Requires an initialized MSVC environment (cl.exe, link.exe, rc.exe, nmake.exe)
# for the host, plus the toolset of every target architecture you ask for.
#
# Usage:
#   ./build.sh              # Build Debug plugin (default architecture: x64)
#   ./build.sh debug        # Build Debug plugin (/MTd)
#   ./build.sh release      # Build Release plugin (/MT, optimized)
#   ./build.sh test         # Build and run unit tests (Debug + CRT Leak Check)
#   ./build.sh release-test # Build and run unit tests against the optimized /MT build
#   ./build.sh asan         # Build and run unit tests with AddressSanitizer
#   ./build.sh analyze      # Run MSVC static code analysis (/analyze)
#   ./build.sh package      # Release build + tests for BOTH architectures, then one install ZIP each
#   ./build.sh zip          # Only create the install ZIPs from existing build\x64 and build\x86
#                           # (ARCH=x64|x86 limits both `package` and `zip` to one architecture)
#   ./build.sh clean        # Clean the build directory of the selected architecture
#   ./build.sh cleanall     # Clean the whole build directory
#   ./build.sh mutate [ids] # Mutation check of the tests (x64, ~10 min); e.g. ./build.sh mutate M5 M53
#
# Architecture selection (environment variable):
#   ARCH=x64|all ./build.sh test    # x64 is the default for every action but `package`
#   ARCH=x86 ./build.sh test        # 32-bit build, artifacts in build\x86
# ==============================================================================
set -euo pipefail

ACTION="${1:-debug}"
ARCH_REQUEST="${ARCH:-}"

# Under MSYS2 / Git Bash, convert POSIX-style drive prefixes (/c/..., /d/...)
# in INCLUDE and LIB to Windows drive prefixes (C:/..., D:/...) required by cl.exe/link.exe.
normalize_win_env_list() {
    local val="${1:-}"
    echo "${val}" | sed -E 's (^|;)/([a-zA-Z])/ \1\U\2:/ g'
}

ORIG_PATH="${PATH}"
ORIG_INCLUDE="$(normalize_win_env_list "${INCLUDE:-}")"
ORIG_LIB="$(normalize_win_env_list "${LIB:-}")"

if [[ -n "${ORIG_INCLUDE}" ]]; then
    export INCLUDE="${ORIG_INCLUDE}"
fi
if [[ -n "${ORIG_LIB}" ]]; then
    export LIB="${ORIG_LIB}"
fi

# ------------------------------------------------------------------------------
# switch_arch <x64|x86>: point the environment at another target architecture.
# The MSVC/bin and SDK/lib folders of the toolset differ only by the
# architecture segment (…\bin\Hostx64\x64, …\lib\x64, …\Lib\<sdk>\um\x64), so the
# paths of the active environment are translated instead of hardcoding a toolset
# location: the repository stays free of machine specific paths.
# ------------------------------------------------------------------------------
switch_arch() {
    local want="$1"
    if [[ "${VSCMD_ARG_TGT_ARCH:-}" == "${want}" ]]; then
        return 0
    fi

    # LIB: every architecture segment is the target one (VC lib\x64, SDK um\x64, ucrt\x64)
    LIB="$(sed -E 's#(^|[;\\/])(x64|x86|arm64|arm)([;\\/]|$)#\1'"${want}"'\3#g' <<< "${ORIG_LIB}")"
    export LIB

    # PATH: only the compiler directory has the target segment (…\bin\Hostx64\<target>);
    # the SDK tools in …\Windows Kits\10\bin\<sdk>\<host> stay the host ones on purpose
    PATH="$(sed -E 's#([\\/]bin[\\/]Host[^\\/:;]*[\\/])(x64|x86|arm64|arm)(:|$)#\1'"${want}"'\3#g' <<< "${ORIG_PATH}")"
    export PATH

    export VSCMD_ARG_TGT_ARCH="${want}"

    if ! command -v cl.exe >/dev/null 2>&1; then
        echo "[build] ERROR: the MSVC toolset for ARCH=${want} is not installed." >&2
        echo "[build]        Install the x86/x64 build tools, then run from an environment" >&2
        echo "[build]        initialized for ${want} (vcvarsall.bat ${want} or setup_${want}.bat)." >&2
        exit 1
    fi
}

# ------------------------------------------------------------------------------
# Object files, the DLL and the test programs belong to one configuration
# (Debug / Release / ASan / Analyze): switching configuration in the same folder
# would silently link objects built for another CRT, so the requested
# configuration is remembered in build/<arch>/.config and a change (or an
# explicit "fresh" request) triggers a clean build of that architecture. The
# stamp is written AFTER the clean, which removes the whole folder; a build
# folder without a stamp is treated as stale as well.
#   prepare_config <arch> <Debug|Release|ASan|Analyze> [fresh]
# ------------------------------------------------------------------------------
prepare_config() {
    local arch="$1" want="$2" fresh="${3:-}" stamp="build/${1}/.config" current=""
    if [[ -f "${stamp}" ]]; then
        current="$(cat "${stamp}")"
    fi
    if [[ -n "${fresh}" || "${current}" != "${want}" ]]; then
        if [[ -d "build/${arch}" ]]; then
            echo "[build] clean ${arch} (${want}${current:+, previous build was ${current}})"
            run_nmake "${arch}" clean
        fi
    fi
    mkdir -p "build/${arch}"
    printf '%s' "${want}" > "${stamp}"
}

plugin_file_for() {
    if [[ "$1" == "x86" ]]; then
        echo "build/x86/restic.wfx"
    else
        echo "build/x64/restic.wfx64"
    fi
}

run_nmake() {
    local arch="$1"
    shift
    local dll
    dll="$(plugin_file_for "${arch}")"
    # If Total Commander currently holds the DLL loaded in memory, Windows
    # forbids overwriting it in place but allows renaming it out of the way.
    if [[ -f "${dll}" ]] && ! rm -f "${dll}" 2>/dev/null; then
        mv -f "${dll}" "${dll}.old" 2>/dev/null || true
    fi
    switch_arch "${arch}"
    MSYS2_ARG_CONV_EXCL="*" nmake -nologo ARCH="${arch}" "$@"
}

# Architectures whose install ZIP is created: both, unless ARCH names a single one
package_arches() {
    if [[ -n "${ARCH_REQUEST}" && "${ARCH_REQUEST}" != "all" ]]; then
        echo "${ARCH_REQUEST}"
    fi
}

# Architectures to visit: an explicit ARCH=all means both, `package` always needs both
arches_for() {
    if [[ "${ARCH_REQUEST}" == "all" || ( -z "${ARCH_REQUEST}" && "$1" == "package" ) ]]; then
        echo "x64 x86"
    elif [[ -n "${ARCH_REQUEST}" ]]; then
        echo "${ARCH_REQUEST}"
    else
        echo "x64"
    fi
}

run_action() {
    local arch="$1"
    case "${ACTION}" in
        debug)
            prepare_config "${arch}" Debug
            run_nmake "${arch}" CONFIG=Debug all
            ;;
        release)
            prepare_config "${arch}" Release
            run_nmake "${arch}" CONFIG=Release all
            ;;
        test)
            prepare_config "${arch}" Debug
            run_nmake "${arch}" CONFIG=Debug test
            ;;
        release-test)
            prepare_config "${arch}" Release
            run_nmake "${arch}" CONFIG=Release test
            ;;
        asan)
            prepare_config "${arch}" ASan
            run_nmake "${arch}" CONFIG=Debug ASAN=1 test
            ;;
        analyze)
            prepare_config "${arch}" Analyze fresh
            run_nmake "${arch}" CONFIG=Debug ANALYZE=1 all test
            ;;
        clean)
            run_nmake "${arch}" clean
            ;;
    esac
}

case "${ACTION}" in
    debug|release|test|release-test|asan|analyze|clean)
        for arch in $(arches_for "${ACTION}"); do
            run_action "${arch}"
        done
        ;;
    package)
        for arch in $(arches_for "${ACTION}"); do
            echo "[build] package ${arch}"
            prepare_config "${arch}" Release fresh
            run_nmake "${arch}" CONFIG=Release test
            run_nmake "${arch}" CONFIG=Release package
        done
        python tools/package.py $(package_arches)
        ;;
    zip)
        python tools/package.py $(package_arches)
        ;;
    mutate)
        # Deliberate defects must be caught by the tests (see tests/mutation/run.sh)
        python tests/mutation/check_mutations.py
        shift
        exec bash tests/mutation/run.sh "$@"
        ;;
    cleanall)
        run_nmake x64 cleanall
        ;;
    *)
        echo "Usage: $0 {debug|release|test|release-test|asan|analyze|package|zip|clean|cleanall|mutate [ids]}" >&2
        echo "       ARCH=x64|x86|all $0 <action>   (package defaults to both architectures)" >&2
        exit 1
        ;;
esac
