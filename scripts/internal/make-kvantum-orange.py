#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
"""Makes KvMojaveLightOrange from Kvantum's KvMojaveLight: the theme's blue
accents (buttons, selections, focus, sliders) become a pale, matte orange
matching the OpsRemote icon; greys and other colours stay as they are."""
import colorsys
import os
import re
import shutil

THEMES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "third_party", "kvantum", "themes")
BASE, NAME = "KvMojaveLight", "KvMojaveLightOrange"
TINT_HUE = colorsys.rgb_to_hls(0xE0 / 255, 0x95 / 255, 0x5A / 255)[0]
HEX = re.compile(r"#([0-9a-fA-F]{6})\b")


def orange(match):
    r, g, b = (int(match.group(1)[i:i + 2], 16) / 255 for i in (0, 2, 4))
    hue, lightness, saturation = colorsys.rgb_to_hls(r, g, b)
    # Only blues (azure to indigo) are accents in this theme.
    if saturation < 0.3 or not 0.52 <= hue <= 0.7:
        return match.group(0)
    hue = (TINT_HUE + (hue - 0.6) * 0.1) % 1.0
    r, g, b = colorsys.hls_to_rgb(hue, min(0.8, lightness * 1.08), 0.25 + saturation * 0.4)
    return "#%02x%02x%02x" % tuple(round(c * 255) for c in (r, g, b))


def main():
    source, target = os.path.join(THEMES, BASE), os.path.join(THEMES, NAME)
    shutil.rmtree(target, ignore_errors=True)
    os.makedirs(target)
    with open(os.path.join(source, BASE + ".svg")) as file:
        svg = HEX.sub(orange, file.read())
    with open(os.path.join(target, NAME + ".svg"), "w") as file:
        file.write(svg)
    with open(os.path.join(source, BASE + ".kvconfig")) as file:
        config = file.read()
    # The selection colour is grey in Mojave Light; make it the accent.
    replacements = {
        "highlight.color=#696969": "highlight.color=#c98a5b",
        "inactive.highlight.color=#5a5a5a": "inactive.highlight.color=#bf9a7e",
        "link.color=#0057AE": "link.color=#b0602a",
        "link.visited.color=#452886": "link.visited.color=#8a5a3c",
    }
    for old, new in replacements.items():
        if old not in config:
            raise SystemExit(f"{BASE}.kvconfig no longer has {old!r}; update this script")
        config = config.replace(old, new)
    with open(os.path.join(target, NAME + ".kvconfig"), "w") as file:
        file.write(HEX.sub(orange, config))
    print(f"Wrote {target}")


if __name__ == "__main__":
    main()
