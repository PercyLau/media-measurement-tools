#!/usr/bin/env bash

# Common GStreamer environment bootstrap for Orion O6 / CIX BSP images.
# Safe to source multiple times.

if [[ -d "/usr/share/cix/lib/gstreamer-1.0" && -z "${GST_PLUGIN_PATH_1_0:-}" ]]; then
  export GST_PLUGIN_PATH_1_0="/usr/share/cix/lib/gstreamer-1.0"
fi

if [[ -f "/usr/share/cix/libexec/gstreamer-1.0/gst-plugin-scanner" && -z "${GST_PLUGIN_SCANNER:-}" ]]; then
  export GST_PLUGIN_SCANNER="/usr/share/cix/libexec/gstreamer-1.0/gst-plugin-scanner"
fi
