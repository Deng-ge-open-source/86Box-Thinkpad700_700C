#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: $0 <extracted-windows-artifact-dir> [output-dir]" >&2
    exit 2
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
artifact_root="$(cd -- "$1" && pwd)"
work_root="$repo_root/local-assets/legacy-work"
runtime_source="$work_root/tp700-runtime"
system_source="$work_root/720cc-system"
timestamp="$(date +%Y%m%d-%H%M%S)"
output_root="${2:-$repo_root/private-dist/ThinkPad700-Windows-x64-private-$timestamp}"

rom_source="$runtime_source/roms/machines/ibmtp700/39G9996.bin"
disk_source="$system_source/TP700-restore-scratch-370.img"
service_source="$system_source/ThinkPad-700-system-partition-restored-3MiB.img"
reference_source="$runtime_source/r700m120-tp700.ima"
diagnostic_source="$runtime_source/dg720120.ima"
config_template="$repo_root/configs/thinkpad700/86box.windows-private.cfg.example"
launcher_template="$repo_root/configs/thinkpad700/Start-ThinkPad-700.bat"
readme_template="$repo_root/docs/PRIVATE-PACKAGE-README.txt"

for required in "$rom_source" "$disk_source" "$service_source" \
                "$reference_source" "$config_template" \
                "$launcher_template" "$readme_template"; do
    if [[ ! -f "$required" ]]; then
        echo "Missing required private asset: $required" >&2
        exit 1
    fi
done

if [[ -e "$output_root" || -e "$output_root.zip" ]]; then
    echo "Refusing to overwrite existing package: $output_root" >&2
    exit 1
fi

windows_exe="$(find "$artifact_root" -type f -iname '86Box.exe' -print -quit)"
if [[ -z "$windows_exe" ]]; then
    echo "86Box.exe was not found below $artifact_root" >&2
    exit 1
fi

# Validate and repair only the private working copies before taking the
# package snapshot. Original archives and source media are never modified.
TP700_WORK_ROOT="$work_root" python3 "$repo_root/tools/thinkpad700/patch_tp700_media.py"

binary_dir="$(dirname -- "$windows_exe")"
mkdir -p "$output_root"
cp -a "$binary_dir/." "$output_root/"

mkdir -p "$output_root/runtime/roms/machines/ibmtp700"
mkdir -p "$output_root/runtime/ThinkPad 700/nvr"
mkdir -p "$output_root/runtime/media"

cp --sparse=always "$rom_source" \
    "$output_root/runtime/roms/machines/ibmtp700/39G9996.bin"
cp --sparse=always "$disk_source" \
    "$output_root/runtime/media/ThinkPad700-system.img"
cp --sparse=always "$service_source" \
    "$output_root/runtime/media/ThinkPad700-service-partition.img"
cp --sparse=always "$reference_source" \
    "$output_root/runtime/media/ThinkPad700-reference-1.20.ima"

if [[ -f "$diagnostic_source" ]]; then
    cp --sparse=always "$diagnostic_source" \
        "$output_root/runtime/media/ThinkPad700-diagnostics.ima"
fi
if [[ -d "$runtime_source/ThinkPad 700/nvr" ]]; then
    cp -a "$runtime_source/ThinkPad 700/nvr/." \
        "$output_root/runtime/ThinkPad 700/nvr/"
fi

cp "$config_template" "$output_root/runtime/ThinkPad 700/86box.cfg"
cp "$launcher_template" "$output_root/Start-ThinkPad-700.bat"
cp "$readme_template" "$output_root/PRIVATE-README.txt"

(
    cd "$output_root"
    find . -type f ! -name SHA256SUMS.txt -print0 |
        sort -z |
        xargs -0 sha256sum > SHA256SUMS.txt
)

(
    cd "$(dirname -- "$output_root")"
    python3 -m zipfile -c "$(basename -- "$output_root").zip" \
        "$(basename -- "$output_root")"
)

echo "Private package directory: $output_root"
echo "Private package archive:   $output_root.zip"
