#!/usr/bin/env bash
set -euo pipefail

# Bootstrap a fresh Ubuntu / Debian machine for this project.
#
# What it does:
#   1. Installs system packages required to build/use PyGObject with GStreamer
#   2. Installs uv if missing
#   3. Runs uv sync in the project root
#   4. Runs a small import smoke test inside the uv environment

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

APT_PACKAGES=(
  build-essential
  curl
  gcc
  gir1.2-gstreamer-1.0
  gobject-introspection
  libgirepository-2.0-dev
  libcairo2-dev
  pkg-config
  python3-dev
  python3-venv
  gstreamer1.0-tools
  gstreamer1.0-plugins-base
  gstreamer1.0-plugins-good
  gstreamer1.0-plugins-bad
  gstreamer1.0-libav
)

echo "[bootstrap_ubuntu_uv] Project root: ${PROJECT_ROOT}"
echo "[bootstrap_ubuntu_uv] Installing system dependencies..."
sudo apt update
sudo apt install -y "${APT_PACKAGES[@]}"

if ! command -v uv >/dev/null 2>&1; then
  echo "[bootstrap_ubuntu_uv] Installing uv..."
  curl -LsSf https://astral.sh/uv/install.sh | sh
  export PATH="${HOME}/.local/bin:${PATH}"
fi

echo "[bootstrap_ubuntu_uv] Syncing Python environment with uv..."
cd "${PROJECT_ROOT}"
uv sync

echo "[bootstrap_ubuntu_uv] Running runtime smoke test..."
uv run python -c "import gi; gi.require_version('Gst', '1.0'); from gi.repository import Gst, GLib; Gst.init(None); print('GI_OK', Gst.version_string())"

echo "[bootstrap_ubuntu_uv] Done."
