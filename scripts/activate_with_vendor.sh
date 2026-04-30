#!/usr/bin/env bash
# Activate project virtualenv and set vendor CIX library/plugin paths for the current shell.
# Usage:  source scripts/activate_with_vendor.sh

_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
PROJECT_ROOT="${_SCRIPT_DIR}/.."

VENDOR_LIB_DIR="/usr/share/cix/lib"
VENDOR_GST_PLUGINS="/usr/share/cix/lib/gstreamer-1.0"

if [ -f "${PROJECT_ROOT}/.venv/bin/activate" ]; then
  # shellcheck disable=SC1090
  . "${PROJECT_ROOT}/.venv/bin/activate"
  echo "Activated virtualenv: ${PROJECT_ROOT}/.venv"
else
  echo "Warning: virtualenv not found at ${PROJECT_ROOT}/.venv — activate manually if needed"
fi

export LD_LIBRARY_PATH="${VENDOR_LIB_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export GST_PLUGIN_PATH_1_0="${VENDOR_GST_PLUGINS}${GST_PLUGIN_PATH_1_0:+:${GST_PLUGIN_PATH_1_0}}"

echo "Set LD_LIBRARY_PATH=${LD_LIBRARY_PATH}"
echo "Set GST_PLUGIN_PATH_1_0=${GST_PLUGIN_PATH_1_0}"

echo "To make the vendor libraries available system-wide, run as root:"
echo "  echo \"${VENDOR_LIB_DIR}\" | sudo tee /etc/ld.so.conf.d/cix.conf && sudo ldconfig"
