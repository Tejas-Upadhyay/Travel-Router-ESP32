#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export IDF_TOOLS_PATH="$PWD/.tools/idf-tools"
export IDF_PYTHON_ENV_PATH="$IDF_TOOLS_PATH/python_env/idf5.5_py3.9_env"
export PATH="$PWD/.tools/venv/bin:$PATH"
# ESP-IDF's export selects its own Python environment for the build.
source .tools/esp-idf/export.sh >/dev/null
exec idf.py "$@"
