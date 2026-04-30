#!/usr/bin/env bash
# Undo environment changes made by scripts/activate_with_vendor.sh
# Usage: source scripts/deactivate_vendor.sh

_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
PROJECT_ROOT="${_SCRIPT_DIR}/.."

VENDOR_LIB_DIR="/usr/share/cix/lib"
VENDOR_GST_PLUGINS="/usr/share/cix/lib/gstreamer-1.0"

remove_path() {
  local varname="$1"; local path="$2"
  eval "val=\"\${${varname}:-}\""
  if [ -z "${val}" ]; then
    return
  fi
  # remove all occurrences of path from colon-separated list
  local new
  new=$(printf "%s" "$val" | awk -v RS=":" -v ORS=":" -v p="$path" '$0!=p' | sed 's/:$//')
  if [ -z "$new" ]; then
    eval "unset $varname"
  else
    eval "export $varname=\"$new\""
  fi
}

# Remove vendor paths from environment variables
remove_path LD_LIBRARY_PATH "$VENDOR_LIB_DIR"
remove_path GST_PLUGIN_PATH_1_0 "$VENDOR_GST_PLUGINS"

# Deactivate virtualenv if available
if [ -n "\${VIRTUAL_ENV:-}" ]; then
  if type deactivate >/dev/null 2>&1; then
    deactivate
    echo "Deactivated virtualenv"
  else
    echo "Virtualenv active at \${VIRTUAL_ENV}, but 'deactivate' not found."
    echo "Open a new shell or run 'deactivate' if available to fully restore PATH."
  fi
fi

echo "Removed ${VENDOR_LIB_DIR} from LD_LIBRARY_PATH and ${VENDOR_GST_PLUGINS} from GST_PLUGIN_PATH_1_0"
