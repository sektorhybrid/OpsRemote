#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Refreshes the curated terminal color schemes vendored in
# third_party/iterm2-color-schemes from iTerm2-Color-Schemes (MIT license).
# The source commit is pinned so the result is reproducible; every file is
# checked to be a plain Konsole-format color scheme before it is kept.
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
TARGET="${PROJECT_DIR}/third_party/iterm2-color-schemes"
REPOSITORY="mbadolato/iTerm2-Color-Schemes"
COMMIT="27cbbb5ee7e98ff15484e7342b07a20606260176"   # release-20261026-153024-27cbbb5
BASE_URL="https://raw.githubusercontent.com/${REPOSITORY}/${COMMIT}"

SCHEMES=(
    "Adwaita" "Adwaita Dark" "Afterglow" "Apple System Colors" "Apple System Colors Light"
    "Atom One Dark" "Atom One Light" "Ayu" "Ayu Light" "Ayu Mirage" "Breeze"
    "Builtin Tango Dark" "Builtin Tango Light" "Carbonfox" "Catppuccin Frappe"
    "Catppuccin Latte" "Catppuccin Macchiato" "Catppuccin Mocha" "Cobalt2" "Dayfox"
    "Doom One" "Dracula" "Dracula+" "Everforest Dark Hard" "Everforest Light Med"
    "Flexoki Dark" "Flexoki Light" "GitHub" "GitHub Dark" "GitHub Dark Default"
    "GitHub Light Default" "Gruvbox Dark" "Gruvbox Light" "Gruvbox Material" "Homebrew"
    "Iceberg Dark" "Iceberg Light" "Jellybeans" "Kanagawa Dragon" "Kanagawa Wave"
    "Material" "Material Dark" "Material Ocean" "Melange Dark" "Melange Light"
    "Monokai Pro" "Monokai Remastered" "Moonfly" "Night Owl" "Night Owlish Light"
    "Nightfox" "Nord Light" "Oceanic Next" "One Half Dark" "One Half Light" "Rose Pine"
    "Rose Pine Dawn" "Rose Pine Moon" "Selenized Dark" "Selenized Light" "Snazzy"
    "Solarized Dark Higher Contrast" "Sonokai" "Spacegray" "Synthwave" "Tomorrow"
    "Tomorrow Night" "Tomorrow Night Bright" "TokyoNight" "TokyoNight Day"
    "TokyoNight Storm" "Vesper" "Zenburn" "iTerm2 Solarized Dark" "iTerm2 Solarized Light"
)

# A scheme must define the background, foreground, and the eight base ANSI
# colors as "Color=r,g,b" entries, and nothing but ini sections and keys.
validate() {
    python3 -I - "$1" <<'PY'
import configparser, re, sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
if len(text) > 65536:
    sys.exit("too large")
parser = configparser.ConfigParser(interpolation=None)
parser.optionxform = str
parser.read_string(text)
required = ["Background", "Foreground"] + [f"Color{i}" for i in range(8)]
for section in required:
    value = parser.get(section, "Color", fallback="")
    if not re.fullmatch(r"\s*\d{1,3}\s*,\s*\d{1,3}\s*,\s*\d{1,3}\s*", value) \
            or any(int(part) > 255 for part in value.split(",")):
        sys.exit(f"missing or invalid [{section}] Color")
PY
}

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
curl -fsSL "${BASE_URL}/LICENSE" -o "${tmp}/LICENSE"
for scheme in "${SCHEMES[@]}"; do
    file="${scheme}.colorscheme"
    url="${BASE_URL}/konsole/$(python3 -I -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "${file}")"
    curl -fsSL "${url}" -o "${tmp}/${file}"
    if ! reason="$(validate "${tmp}/${file}" 2>&1)"; then
        echo "Rejected ${file}: ${reason}" >&2
        exit 1
    fi
done

rm -rf "${TARGET}"
mkdir -p "${TARGET}"
mv "${tmp}"/*.colorscheme "${tmp}/LICENSE" "${TARGET}/"
cat > "${TARGET}/README.md" <<README
# Terminal color schemes from iTerm2-Color-Schemes

A curated selection of ${#SCHEMES[@]} terminal color schemes in Konsole
format, taken unmodified from https://github.com/${REPOSITORY}
at commit ${COMMIT} (directory \`konsole/\`). They are licensed under the MIT
license in \`LICENSE\`.

Refresh or change the selection with \`scripts/internal/update-color-schemes.sh\`; it
validates every file before keeping it.
README
echo "Vendored ${#SCHEMES[@]} color schemes into ${TARGET}"
