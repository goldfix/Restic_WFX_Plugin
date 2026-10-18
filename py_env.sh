#!/usr/bin/env bash
# Helper script to create and activate virtual environment using uv

# Determine directory of this script (compatible with bash and zsh)
if [ -n "${ZSH_VERSION:-}" ]; then
    _SCRIPT_SRC="${(%):-%x}"
else
    _SCRIPT_SRC="${BASH_SOURCE[0]:-$0}"
fi
SCRIPT_DIR="$(cd "$(dirname "${_SCRIPT_SRC}")" && pwd)"
ACTION="${1:-}"
PYTHON_VERSION="${2:-"3.12"}"
VENV_DIR="${SCRIPT_DIR}/.venv"

usage() {
    echo "Usage: source py_env.sh [init|active|activate] [python_version]"
    echo ""
    echo "Commands:"
    echo "  init [version]      - Remove existing environment and create fresh .venv (default: 3.12)"
    echo "  active | activate   - Activate existing .venv and load environment variables"
    echo ""
    echo "Examples:"
    echo "  source py_env.sh init 3.12"
    echo "  source py_env.sh init 3.11"
    echo "  source py_env.sh active"
    echo "  source py_env.sh activate"
}

# Warn if script is executed directly instead of sourced
if [ -n "${BASH_VERSION:-}" ] && [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    echo "Warning: py_env.sh is being executed instead of sourced."
    echo "To activate the environment in your current shell, run:"
    echo "  source py_env.sh [init|active|activate] [python_version]"
    echo ""
fi

# Show usage if no command provided or help requested
if [[ -z "${ACTION}" || "${ACTION}" == "-h" || "${ACTION}" == "--help" || "${ACTION}" == "help" ]]; then
    usage
    return 1 2>/dev/null || exit 1
fi

# 1. Carica le variabili da .env subito, in modo che PATH/MSVC siano disponibili per uv/build
if [ -f "${SCRIPT_DIR}/.env" ]; then
    set -a
    source "${SCRIPT_DIR}/.env"
    set +a
    echo "--> Environment variables loaded from .env"
fi

# Helper function to activate venv across Windows (Scripts/activate) and Unix (bin/activate)
_activate_venv() {
    local ACTIVATE_SCRIPT=""
    if [ -f "${VENV_DIR}/Scripts/activate" ]; then
        ACTIVATE_SCRIPT="${VENV_DIR}/Scripts/activate"
    elif [ -f "${VENV_DIR}/bin/activate" ]; then
        ACTIVATE_SCRIPT="${VENV_DIR}/bin/activate"
    fi

    if [ -n "${ACTIVATE_SCRIPT}" ]; then
        # shellcheck source=/dev/null
        source "${ACTIVATE_SCRIPT}"
        echo "--> Virtual environment (.venv) activated."
        return 0
    else
        echo "--> Error: Activation script not found in ${VENV_DIR}/Scripts/activate or ${VENV_DIR}/bin/activate."
        return 1
    fi
}

# Helper to load py_var.sh if present
_load_py_var() {
    if [ -f "${SCRIPT_DIR}/py_var.sh" ]; then
        # shellcheck source=/dev/null
        source "${SCRIPT_DIR}/py_var.sh"
        echo "--> Environment variables loaded from py_var.sh"
    fi
}

if [[ "${ACTION}" == "init" || "${ACTION}" == "init_py" ]]; then
    if ! command -v uv > /dev/null 2>&1; then
        echo "Error: 'uv' is not installed in system."
        echo "Please install uv (e.g. 'brew install uv' or 'curl -LsSf https://astral.sh/uv/install.sh | sh')."
        return 1 2>/dev/null || exit 1
    fi

    echo "--> Creating fresh virtual environment in .venv using Python ${PYTHON_VERSION} (uv --clear removes any existing one)..."
    if ! uv venv --clear --python "${PYTHON_VERSION}" "${VENV_DIR}"; then
        echo "--> Error: 'uv venv' failed for Python ${PYTHON_VERSION}."
        return 1 2>/dev/null || exit 1
    fi

    if ! _activate_venv; then
        return 1 2>/dev/null || exit 1
    fi

    echo "--> Syncing project dependencies (including dev group) with uv..."
    if ! uv sync --group dev --python "${PYTHON_VERSION}"; then
        echo "--> Error: 'uv sync' failed for Python ${PYTHON_VERSION}."
        echo "--> Check that 'requires-python' in pyproject.toml allows this version."
        return 1 2>/dev/null || exit 1
    fi

    _load_py_var

elif [[ "${ACTION}" == "active" || "${ACTION}" == "activate" ]]; then
    if [ -d "${VENV_DIR}" ]; then
        if ! _activate_venv; then
            return 1 2>/dev/null || exit 1
        fi
    else
        echo "--> No virtual environment found at .venv. Run 'source py_env.sh init [version]' first."
        return 1 2>/dev/null || exit 1
    fi

    _load_py_var

else
    usage
    return 1 2>/dev/null || exit 1
fi
