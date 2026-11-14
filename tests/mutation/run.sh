#!/usr/bin/env bash
# ==============================================================================
# Mutation check harness (see AGENT.md, section 8.9).
#
# For every entry of tests/mutation/mutations.txt the harness
#   1. applies one deliberate defect to the source (mut_mNN.py apply),
#   2. rebuilds only the affected binary (Debug, x64),
#   3. runs the matching test program and requires it to FAIL,
#   4. restores the source (mut_mNN.py revert).
# A mutation the tests do not notice is reported as [MISSED] and fails the run.
#
# Usage (from anywhere):
#   ./build.sh mutate             # every mutation (~10 minutes)
#   ./build.sh mutate M5 M53      # only the listed ids
#   bash tests/mutation/run.sh    # same thing, without the build.sh wrapper
#
# Mutations are always built as x64 Debug: the tested logic is architecture independent.
# Source files are restored after every mutation, also when the build or the run fails;
# if the harness is killed in the middle, `git checkout -- <file>` restores the source
# (the original is also kept next to it as <file>.mutbak).
# ==============================================================================
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${HERE}/../.." || exit 1
export ARCH=x64

TABLE="tests/mutation/mutations.txt"
LOG_DIR="$(mktemp -d)"
FIXTURE="tmp/test_fixture"
PRISTINE="tmp/test_fixture.pristine"
trap 'rm -rf "${LOG_DIR}" "${PRISTINE}"' EXIT

filter="$*"
wanted() {
    [ -z "${filter}" ] && return 0
    case " ${filter} " in
        *" $1 "*) return 0 ;;
        *) return 1 ;;
    esac
}

# The harness relinks one target at a time with the Debug configuration, so it starts from a
# clean Debug build: a Release build left in build/x64 would be silently reused.
./build.sh clean >/dev/null 2>&1 || true
./build.sh debug >/dev/null 2>&1 || { echo "[SETUP-FAIL] Debug build"; exit 1; }

# The DLL smoke test needs the live fixture that test_restic_cli creates (and rebuilds from scratch on
# every run). Create it once with the unmutated program and keep a pristine copy: a cli mutation that is
# detected in the middle of its live test leaves a half-built fixture behind, and the DLL test would then
# skip its end-to-end part instead of exercising the mutation.
MSYS2_ARG_CONV_EXCL="*" nmake /nologo ARCH=x64 'build\x64\test_restic_cli.exe' >/dev/null 2>&1
./build/x64/test_restic_cli.exe >/dev/null 2>&1
if [ ! -f "${FIXTURE}/repo/config" ]; then
    echo "[SETUP-FAIL] the live fixture could not be created (is tmp/restic.exe present?)"
    exit 1
fi
rm -rf "${PRISTINE}"
cp -a "${FIXTURE}" "${PRISTINE}" || { echo "[SETUP-FAIL] fixture backup"; exit 1; }

restore_fixture() {
    rm -rf "${FIXTURE}" && cp -a "${PRISTINE}" "${FIXTURE}"
}

# kind -> "nmake target|executable"
target_for() {
    case "$1" in
        dll)    echo 'build\x64\restic.wfx64 build\x64\test_plugin_dll.exe|build/x64/test_plugin_dll.exe' ;;
        cli)    echo 'build\x64\test_restic_cli.exe|build/x64/test_restic_cli.exe' ;;
        config) echo 'build\x64\test_config.exe|build/x64/test_config.exe' ;;
        stats)  echo 'build\x64\test_stats.exe|build/x64/test_stats.exe' ;;
        report) echo 'build\x64\test_report.exe|build/x64/test_report.exe' ;;
        *)      echo "" ;;
    esac
}

run_mutation() {
    local id="$1" kind="$2" script="tests/mutation/$3" name="$4" spec target exe
    spec="$(target_for "${kind}")"
    if [ -z "${spec}" ]; then
        echo "[SETUP-FAIL] ${id} unknown kind '${kind}'"
        return 1
    fi
    target="${spec%%|*}"
    exe="${spec##*|}"

    if [ "${kind}" = "dll" ]; then
        restore_fixture || { echo "[SETUP-FAIL] ${id} fixture restore"; return 1; }
    fi
    python "${script}" apply || { echo "[SETUP-FAIL] ${id} ${name}"; return 1; }
    # ${target} is intentionally unquoted: the DLL kind names two nmake targets (DLL + smoke test)
    # shellcheck disable=SC2086
    if ! MSYS2_ARG_CONV_EXCL="*" nmake /nologo ARCH=x64 ${target} >"${LOG_DIR}/build.log" 2>&1; then
        echo "[BUILD-FAIL] ${id} ${name}"
        tail -5 "${LOG_DIR}/build.log"
        python "${script}" revert
        return 1
    fi
    # Stop at the first failed assertion: a detected mutation does not need the rest of the run
    python tests/mutation/run_until_fail.py "./${exe}" >"${LOG_DIR}/run.log" 2>&1
    local rc=$?
    python "${script}" revert
    case ${rc} in
        0) echo "[MISSED] ${id} ${name} NOT detected"; return 1 ;;
        1) echo "[OK]     ${id} ${name} detected -> $(head -1 "${LOG_DIR}/run.log")"; return 0 ;;
        3) echo "[SKIPPED] ${id} ${name}: the tests that guard it were skipped -> $(head -1 "${LOG_DIR}/run.log")"; return 1 ;;
        *) echo "[RUN-FAIL] ${id} ${name}: the test program could not be run -> $(head -1 "${LOG_DIR}/run.log")"; return 1 ;;
    esac
}

fail=0
ran=0
while IFS='|' read -r id kind script name; do
    case "${id}" in ''|'#'*) continue ;; esac
    wanted "${id}" || continue
    ran=$((ran + 1))
    run_mutation "${id}" "${kind}" "${script}" "${name}" || fail=1
done < "${TABLE}"

if [ "${ran}" -eq 0 ]; then
    echo "[FAIL] no mutation matched: ${filter}"
    exit 1
fi
if [ ${fail} -eq 0 ]; then
    echo "[PASS] all ${ran} mutations detected"
else
    echo "[FAIL] some mutations survived or could not run"
fi
exit ${fail}
