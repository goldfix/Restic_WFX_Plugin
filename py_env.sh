#!/usr/bin/env bash
# Helper script to create and activate virtual environment using uv

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_VERSION="${2:-"3.12"}"
VENV_DIR="${SCRIPT_DIR}/.venv"


usage() {
    echo "Usage: source py_env.sh [init|active] [python_version]"
    echo ""
    echo "Commands:"
    echo "  init [version]   - Remove existing environment and create fresh .venv (default: 3.12)"
    echo "  active           - Activate existing .venv and load py_var.sh"
    echo ""
    echo "Examples:"
    echo "  source py_env.sh init 3.12"
    echo "  source py_env.sh init 3.11"
    echo "  source py_env.sh active"
}


if ! command -v uv &> /dev/null; then
    echo "Error: 'uv' is not installed in system."
    echo "Please install uv (e.g. 'brew install uv' or 'curl -LsSf https://astral.sh/uv/install.sh | sh')."
    return 1 2>/dev/null || exit 1
fi

if [[ "$1" == "init" || "$1" == "init_py" ]]; then
    echo "--> Creating fresh virtual environment in .venv using Python ${PYTHON_VERSION} (uv --clear removes any existing one)..."
    uv venv --clear --python "${PYTHON_VERSION}" "${VENV_DIR}"

    source "${VENV_DIR}/bin/activate"

    echo "--> Syncing project dependencies (including dev group) with uv..."
    uv sync --group dev --python "${PYTHON_VERSION}"
    SYNC_STATUS=$?
    if [ ${SYNC_STATUS} -ne 0 ]; then
        echo "--> Error: 'uv sync' failed for Python ${PYTHON_VERSION}."
        echo "--> Check that 'requires-python' in pyproject.toml allows this version."
        return 1 2>/dev/null || exit 1
    fi

    if [ -f "${SCRIPT_DIR}/py_var.sh" ]; then
        source "${SCRIPT_DIR}/py_var.sh"
        echo "--> Environment variables loaded from py_var.sh"
    fi

elif [[ "$1" == "active" ]]; then
    if [ -d "${VENV_DIR}" ]; then
        source "${VENV_DIR}/bin/activate"
        echo "--> Virtual environment (.venv) activated."
    else
        echo "--> No virtual environment found at .venv. Run 'source py_env.sh init [version]' first."
        return 1 2>/dev/null || exit 1
    fi

    if [ -f "${SCRIPT_DIR}/py_var.sh" ]; then
        source "${SCRIPT_DIR}/py_var.sh"
        echo "--> Environment variables loaded from py_var.sh"
    fi

else
    usage
fi

# 1. Carica le variabili da .env subito, in modo che PATH/MSVC siano disponibili per uv/build
if [ -f "${SCRIPT_DIR}/.env" ]; then
    set -a
    source "${SCRIPT_DIR}/.env"
    set +a
    echo "--> Environment variables loaded from .env"
fi
