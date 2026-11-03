#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
"""Makes the orange Kvantum themes from their originals (VARIANTS below): the
themes' blue accents (buttons, selections, focus, sliders, links) become a
pale, matte orange matching the OpsRemote icon; greys and other colours stay
as they are. Themes with a grey selection get the accent there too."""
import colorsys
import os
import re
import shutil

THEMES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "third_party", "kvantum", "themes")
TINT_HUE = colorsys.rgb_to_hls(0xE0 / 255, 0x95 / 255, 0x5A / 255)[0]
# #rrggbb, or #rrggbbaa (the alpha is kept).
HEX = re.compile(r"#([0-9a-fA-F]{6})([0-9a-fA-F]{2})?\b")

# base theme, orange theme, exact kvconfig replacements (each must exist).
VARIANTS = [
    ("KvMojaveLight", "KvMojaveLightOrange", {
        # The selection colour is grey in Mojave Light; make it the accent.
        "highlight.color=#696969": "highlight.color=#c98a5b",
        "inactive.highlight.color=#5a5a5a": "inactive.highlight.color=#bf9a7e",
        "link.color=#0057AE": "link.color=#b0602a",
        "link.visited.color=#452886": "link.visited.color=#8a5a3c",
    }),
    ("KvMojave", "KvMojaveDarkOrange", {
        # Mojave (dark) selects in light grey with black text. Every
        # selection (list rows, the Looks list, selected text) becomes the
        # grey of a row under the mouse, with light text, like WhiteSur Dark.
        "highlight.color=#c3c3c6": "highlight.color=#494949",
        "inactive.highlight.color=#adadaf": "inactive.highlight.color=#494949",
        "highlight.text.color=black": "highlight.text.color=#fafafa",
        "inactive.highlight.text.color=#141414": "inactive.highlight.text.color=#e1e1e1",
        # Lists: a selected row looks like a row under the mouse (that dark
        # grey, below), as in WhiteSur Dark, so its text is light too.
        "[ItemView] text.press.color=black": "text.press.color=#fafafa",
        "[ItemView] text.toggle.color=black": "text.toggle.color=#fafafa",
        "[ItemView] text.toggle.inactive.color=#191919": "text.toggle.inactive.color=#e1e1e1",
    }),
    # WhiteSur's selections and links are already its blue accent.
    ("WhiteSur-opaque", "WhiteSur-opaqueOrange", {}),
    # Its light blue selection (white text on it) would turn into a pale
    # peach; a deeper orange keeps the text readable.
    ("WhiteSur-opaqueDark", "WhiteSur-opaqueDarkOrange", {
        "highlight.color=#a0b4f8": "highlight.color=#c27a48",
        "inactive.highlight.color=#a0b4f8": "inactive.highlight.color=#a8734f",
    }),
]
# SVG changes for one theme: (element ids starting with, old, new).
SVG_CHANGES = {
    "KvMojaveDarkOrange": [
        # Selected list rows (pressed, toggled) were light grey; the row under
        # the mouse shows as #494949 (that grey at 12%, drawn as frame and
        # interior both; measured on screen). Both become #494949, and the
        # hover is drawn at full opacity.
        ("itemview-pressed", "fill:#c3c3c6", "fill:#494949"),
        ("itemview-toggled", "fill:#9e9ea0", "fill:#494949"),
        ("itemview-focused", "opacity:.12", "opacity:1"),
    ],
}
# Files kept unchanged next to a theme (licence and authors).
KEEP = ["LICENSE", "AUTHORS"]


def orange(match):
    digits, alpha = match.group(1), match.group(2) or ""
    r, g, b = (int(digits[i:i + 2], 16) / 255 for i in (0, 2, 4))
    hue, lightness, saturation = colorsys.rgb_to_hls(r, g, b)
    # Only blues (azure to indigo) are accents in these themes.
    if saturation < 0.3 or not 0.52 <= hue <= 0.7:
        return match.group(0)
    hue = (TINT_HUE + (hue - 0.6) * 0.1) % 1.0
    r, g, b = colorsys.hls_to_rgb(hue, min(0.8, lightness * 1.08), 0.25 + saturation * 0.4)
    return "#%02x%02x%02x" % tuple(round(c * 255) for c in (r, g, b)) + alpha


def make(base, name, replacements):
    source, target = os.path.join(THEMES, base), os.path.join(THEMES, name)
    shutil.rmtree(target, ignore_errors=True)
    os.makedirs(target)
    with open(os.path.join(source, base + ".svg")) as file:
        svg = file.read()
    for prefix, old, new in SVG_CHANGES.get(name, []):
        # In each element whose id starts with prefix (one tag each).
        element = re.compile(r"<[^<>]*\bid=\"" + re.escape(prefix) + r"[^\"]*\"[^<>]*>")
        changed = [0]

        def change(match):
            if old in match.group(0):
                changed[0] += 1
            return match.group(0).replace(old, new)
        svg = element.sub(change, svg)
        if not changed[0]:
            raise SystemExit(f"{base}.svg has no {old!r} in {prefix}*; update this script")
    svg = HEX.sub(orange, svg)
    with open(os.path.join(target, name + ".svg"), "w") as file:
        file.write(svg)
    with open(os.path.join(source, base + ".kvconfig")) as file:
        config = file.read()
    for old, new in replacements.items():
        # "[Section] key=value" only in that section.
        section = None
        if old.startswith("["):
            section, old = old[1:].split("] ", 1)
        # Whole lines: "highlight.color=..." is also the end of
        # "inactive.highlight.color=...".
        line = re.compile(r"^" + re.escape(old) + r"$", re.M)
        if section is None:
            parts = [config]
        else:
            start = config.find(f"[{section}]\n")
            if start < 0:
                raise SystemExit(f"{base}.kvconfig has no [{section}]; update this script")
            following = config.find("\n[", start + 1)
            end = following + 1 if following >= 0 else len(config)
            parts = [config[:start], config[start:end], config[end:]]
        scope = parts[0] if section is None else parts[1]
        if not line.search(scope):
            raise SystemExit(f"{base}.kvconfig no longer has {old!r}; update this script")
        scope = line.sub(new, scope)
        config = scope if section is None else parts[0] + scope + parts[2]
    with open(os.path.join(target, name + ".kvconfig"), "w") as file:
        file.write(HEX.sub(orange, config))
    for kept in KEEP:
        if os.path.exists(os.path.join(source, kept)):
            shutil.copy(os.path.join(source, kept), target)
    if os.path.exists(os.path.join(source, "SOURCE.txt")):
        with open(os.path.join(source, "SOURCE.txt")) as file:
            origin = file.read()
        with open(os.path.join(target, "SOURCE.txt"), "w") as file:
            file.write(f"{name}: {base} with its blue accents made orange by "
                       f"scripts/internal/make-kvantum-orange.py (OpsRemote, 2026).\n"
                       f"{base} itself:\n{origin}")
    print(f"Wrote {target}")


def main():
    for base, name, replacements in VARIANTS:
        make(base, name, replacements)


if __name__ == "__main__":
    main()
