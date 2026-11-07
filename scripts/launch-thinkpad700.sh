#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
work_root="${TP700_WORK_ROOT:-$repo_root/local-assets/legacy-work}"
runtime_root="$work_root/tp700-runtime"
emulator="$repo_root/build/tp700-base2/src/86Box"
config="$runtime_root/ThinkPad 700/86box.cfg"
library_root="$work_root/toolchain/root/usr/lib"
media_patcher="$repo_root/tools/thinkpad700/patch_tp700_media.py"
launcher_lock="$runtime_root/.launcher.lock"
panel_runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/thinkpad700c-emulator"
launch_log="$runtime_root/LOG-last-launch.log"

export LD_LIBRARY_PATH="$library_root/aarch64-linux-gnu:$library_root:${LD_LIBRARY_PATH:-}"
export TP700_WORK_ROOT="$work_root"
mkdir -p "$panel_runtime"
export TP700_PANEL_STATUS="$panel_runtime/panel-status.json"
export TP700_SLIDER_STATUS="/run/thinkpad700c-leds/slider-status.json"
export TP700_SWITCH_STATUS="/run/thinkpad700c-leds/switch-status.json"

# The physical inversion switch is consumed by this GNOME extension.  Enabling
# an already-enabled extension is harmless and makes a desktop shortcut launch
# self-contained after a shell-extension reset.
if command -v gnome-extensions >/dev/null 2>&1; then
  gnome-extensions enable thinkpad700c-display-modes@codex.local >/dev/null 2>&1 || true
fi

# Media validation takes several seconds.  Keep a second desktop double-click
# from patching the same working images or opening a duplicate VM.
exec 9>"$launcher_lock"
flock --nonblock 9 || exit 0

python3 "$media_patcher"

exec "$emulator" \
  -N \
  -W \
  -F \
  -L "$launch_log" \
  -R "$runtime_root" \
  "$config" \
  >/dev/null 2>&1
