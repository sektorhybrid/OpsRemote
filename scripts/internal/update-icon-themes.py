#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
"""Refreshes the icon themes vendored in third_party/icon-themes.

For each of the pinned upstream icon themes below, only the icons the
application uses are extracted (toolbar and menu actions, folders, and SFTP
file types). Each icon is checked to be a plain, single SVG drawing (no
scripts, event handlers, external references, or entity declarations) before
it is kept, and the theme's license files are copied alongside.

Usage: scripts/internal/update-icon-themes.py [--cache DIR]
  --cache DIR  keep downloaded source archives in DIR between runs
"""
import argparse
import colorsys
import io
import os
import re
import shutil
import sys
import tarfile
import tempfile
import urllib.request
import xml.etree.ElementTree as ElementTree

PROJECT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TARGET = os.path.join(PROJECT, "third_party", "icon-themes")

# id, display name, source archive, release, search roots (in order of
# preference), license files, SPDX summary.
THEMES = [
    ("Breeze", "Breeze", "https://codeload.github.com/KDE/breeze-icons/tar.gz/refs/tags/v6.31.0",
     "v6.31.0", ["icons/"], ["COPYING-ICONS", "COPYING.LIB"], "LGPL-3.0-or-later"),
    ("Papirus", "Papirus",
     "https://codeload.github.com/PapirusDevelopmentTeam/papirus-icon-theme/tar.gz/refs/tags/20260801",
     "20260801", ["Papirus/"], ["LICENSE", "AUTHORS"], "GPL-3.0-only"),
    ("Papirus-Dark", "Papirus Dark",
     "https://codeload.github.com/PapirusDevelopmentTeam/papirus-icon-theme/tar.gz/refs/tags/20260801",
     "20260801", ["Papirus-Dark/", "Papirus/"], ["LICENSE", "AUTHORS"], "GPL-3.0-only"),
    ("Adwaita", "Adwaita",
     "https://gitlab.gnome.org/GNOME/adwaita-icon-theme/-/archive/51.0/adwaita-icon-theme-51.0.tar.gz",
     "51.0", ["Adwaita/"], ["COPYING", "COPYING_LGPL", "AUTHORS"],
     "LGPL-3.0 (dual-licensed with CC-BY-SA-3.0; used under LGPL-3.0)"),
    ("Yaru", "Yaru", "https://codeload.github.com/ubuntu/yaru/tar.gz/refs/tags/r29",
     "r29", ["icons/Suru/", "icons/src/fullcolor/"], ["icons/COPYING", "icons/LICENSE_CCBYSA",
                                                      "icons/LICENSE_GPL3"],
     "CC-BY-SA-4.0 / GPL-3.0"),
    ("Tela", "Tela", "https://codeload.github.com/vinceliuice/Tela-icon-theme/tar.gz/refs/tags/2026-07-07",
     "2026-07-07", ["src/"], ["COPYING", "AUTHORS"], "GPL-3.0-only"),
    ("Fluent", "Fluent",
     "https://codeload.github.com/vinceliuice/Fluent-icon-theme/tar.gz/refs/tags/2026-07-27",
     "2026-07-27", ["src/"], ["COPYING", "AUTHORS"], "GPL-3.0-only"),
    ("Colloid", "Colloid",
     "https://codeload.github.com/vinceliuice/Colloid-icon-theme/tar.gz/refs/tags/2026-08-10",
     "2026-08-10", ["src/"], ["LICENSE"], "GPL-3.0-only"),
    ("Qogir", "Qogir", "https://codeload.github.com/vinceliuice/Qogir-icon-theme/tar.gz/refs/tags/2025-02-15",
     "2025-02-15", ["src/"], ["COPYING", "AUTHORS"], "GPL-3.0-only"),
    ("WhiteSur", "WhiteSur",
     "https://codeload.github.com/vinceliuice/WhiteSur-icon-theme/tar.gz/refs/tags/2026-09-10",
     "2026-09-10", ["src/"], ["COPYING", "AUTHORS"], "GPL-3.0-only"),
    ("elementary", "elementary", "https://codeload.github.com/elementary/icons/tar.gz/refs/tags/9.0.0",
     "9.0.0", [""], ["COPYING"], "GPL-3.0-only"),
]

# Canonical name used by the application -> (context, upstream names to try).
ACTIONS = {
    "document-new": ["document-new"],
    "folder-new": ["folder-new"],
    "media-playback-start": ["media-playback-start", "system-run"],
    "go-up": ["go-up"],
    "go-home": ["go-home", "user-home"],
    "view-refresh": ["view-refresh"],
    "edit-delete": ["edit-delete"],
    "edit-rename": ["edit-rename", "document-edit", "edit-entry"],
    "edit-copy": ["edit-copy"],
    "edit-paste": ["edit-paste"],
    "edit-find": ["edit-find", "system-search"],
    "edit-clear": ["edit-clear-all", "edit-clear"],
    "document-save": ["document-save"],
    "document-open": ["document-open"],
    "document-import": ["document-import", "document-open"],
    "document-export": ["document-export", "document-save-as"],
    "go-down": ["go-down", "download"],
    "upload": ["document-send", "go-up"],
    "preferences-system": ["preferences-system", "configure", "preferences-other"],
    "application-exit": ["application-exit", "system-shutdown"],
    "object-locked": ["object-locked", "system-lock-screen", "changes-prevent"],
    "object-unlocked": ["object-unlocked", "changes-allow", "dialog-password"],
    "help-about": ["help-about"],
    "zoom-in": ["zoom-in"],
    "zoom-out": ["zoom-out"],
    "zoom-original": ["zoom-original", "zoom-fit-best"],
    "process-stop": ["process-stop", "dialog-cancel"],
    "list-add": ["list-add"],
}
APPS = {
    "utilities-terminal": ["utilities-terminal", "terminal", "org.gnome.Terminal", "konsole"],
    # Network tools (ping, traceroute, port scan): a network port or a monitor,
    # not the globe that SSH Tunnels already shows.
    "network-wired": ["preferences-system-network", "utilities-system-monitor",
                      "org.gnome.SystemMonitor", "ksysguard"],
}
PLACES = {
    "folder": ["folder"],
    "folder-remote": ["folder-remote", "folder-network"],
    "user-home": ["user-home", "folder-home"],
    "network-server": ["network-server", "network-workgroup", "computer"],
    "computer": ["computer"],
}
MIMETYPE_NAMES = """
application-excel application-gzip application-java-archive application-json application-msword
application-octet-stream application-pdf application-pkcs10 application-pkix-cert application-sql
application-toml application-vnd.debian.binary-package application-vnd.ms-powerpoint
application-vnd.oasis.opendocument.presentation application-vnd.oasis.opendocument.spreadsheet
application-vnd.oasis.opendocument.text
application-vnd.openxmlformats-officedocument.presentationml.presentation
application-vnd.openxmlformats-officedocument.spreadsheetml.sheet
application-vnd.openxmlformats-officedocument.wordprocessingml.document application-vnd.rar
application-x-7z-compressed application-x-archive application-x-bzip2 application-x-compressed-tar
application-x-desktop application-x-msdownload application-x-object application-x-perl
application-x-php application-x-qemu-disk application-x-rpm application-x-ruby
application-x-sharedlib application-x-tar application-x-xz application-x-xz-compressed-tar
application-xml application-yaml application-zip audio-flac audio-mpeg audio-ogg audio-vnd.wave
image-bmp image-gif image-jpeg image-png image-svg+xml image-vnd.microsoft.icon image-webp
text-css text-csv text-html text-javascript text-markdown text-plain text-rust text-x-c++hdr
text-x-c++src text-x-chdr text-x-csharp text-x-csrc text-x-dockerfile text-x-go text-x-java
text-x-kotlin text-x-log text-x-lua text-x-makefile text-x-patch text-x-python text-x-readme
text-x-scss text-x-shellscript video-mp4 video-quicktime video-webm video-x-matroska
application-x-executable application-x-generic audio-x-generic image-x-generic media-optical
package-x-generic text-x-generic text-x-script video-x-generic x-office-document
x-office-presentation x-office-spreadsheet inode-symlink unknown
""".split()
MIMETYPES = {name: [name] for name in MIMETYPE_NAMES}
MIMETYPES["application-x-shellscript"] = ["application-x-shellscript", "text-x-script"]
MIMETYPES["text-x-shellscript"] = ["text-x-shellscript", "application-x-shellscript", "text-x-script"]
MIMETYPES["inode-symlink"] = ["inode-symlink", "emblem-symbolic-link"]
MIMETYPES["unknown"] = ["unknown", "application-octet-stream", "text-x-generic"]

CONTEXTS = [
    ("actions", ACTIONS, ["actions"]),
    ("apps", APPS, ["apps"]),
    ("places", PLACES, ["places"]),
    ("mimetypes", MIMETYPES, ["mimetypes", "mimes"]),
]
# Directory names preferred for each context (first is best).
SIZE_PREFERENCE = {
    "actions": ["22", "24", "16", "32", "scalable", "48", "symbolic"],
    "apps": ["scalable", "64", "48", "96", "128", "32", "24", "symbolic"],
    "places": ["scalable", "64", "48", "96", "128", "32", "24", "22", "16", "symbolic"],
    "mimetypes": ["scalable", "64", "48", "96", "128", "32", "24", "22", "16", "symbolic"],
}
SKIP_SEGMENTS = {"colors", "bold", "notint", "links", "alternative", "icons-dark", "@2x",
                 "Papirus-Light", "ePapirus", "ePapirus-Dark"}
MAXIMUM_SVG_SIZE = 256 * 1024
SVG_NAMESPACE = "{http://www.w3.org/2000/svg}"


def size_key(segment):
    match = re.fullmatch(r"(\d+)(x\d+)?", segment)
    return match.group(1) if match else segment


def check_svg(data):
    """Returns None if the SVG is a safe single drawing, else the reason."""
    if len(data) > MAXIMUM_SVG_SIZE:
        return "too large"
    text = data.decode("utf-8", errors="strict")
    if re.search(r"<!(DOCTYPE|ENTITY)", text, re.IGNORECASE):
        return "has a DOCTYPE or entity declaration"
    root = ElementTree.fromstring(data)
    if root.tag != SVG_NAMESPACE + "svg":
        return "not an SVG document"
    for element in root.iter():
        tag = element.tag.split("}")[-1].lower()
        if tag in ("script", "foreignobject", "iframe", "embed", "object", "audio", "video"):
            return f"contains <{tag}>"
        for attribute, value in element.attrib.items():
            name = attribute.split("}")[-1].lower()
            if name.startswith("on"):
                return f"has event handler {name}"
            if name == "href" and not (value.startswith("#") or value.startswith("data:image/")):
                return "references an external resource"
        if tag == "style" and element.text and re.search(r"@import|url\(\s*['\"]?(?!#)",
                                                         element.text):
            return "style sheet references an external resource"
    # Reject multi-icon source sheets: an icon is (close to) square.
    width, height = root.get("width", ""), root.get("height", "")
    view_box = root.get("viewBox", "").replace(",", " ").split()
    try:
        if len(view_box) == 4:
            width, height = float(view_box[2]), float(view_box[3])
        else:
            width, height = float(re.sub(r"[a-z%]+$", "", width)), float(re.sub(r"[a-z%]+$", "", height))
    except ValueError:
        return "has no usable size"
    if width <= 0 or height <= 0 or not 0.8 <= width / height <= 1.25 or width > 512:
        return "is not a single square icon (source sheet?)"
    return None


def index_theme(members, roots):
    """Maps icon base names to candidate (score parts, path) per search root."""
    candidates = {}
    for path in members:
        if not path.endswith(".svg"):
            continue
        for root_rank, root in enumerate(roots):
            if not path.startswith(root):
                continue
            segments = path[len(root):].split("/")
            if any(segment in SKIP_SEGMENTS for segment in segments):
                break
            base = segments[-1][:-4]
            candidates.setdefault(base, []).append((root_rank, segments[:-1], path))
            break
    return candidates


def choose(candidates, names, context_dirs, context):
    """Picks the best file for the first upstream name a theme provides."""
    preference = SIZE_PREFERENCE[context]
    for name in names:
        options = []
        for variant, symbolic in ((name, False), (name + "-symbolic", True)):
            for root_rank, directories, path in candidates.get(variant, []):
                if not any(directory in context_dirs for directory in directories):
                    continue
                sizes = [size_key(directory) for directory in directories]
                rank = min((preference.index(size) for size in sizes if size in preference),
                           default=len(preference))
                options.append((root_rank, symbolic, rank, path))
        if options:
            return sorted(options)
    return []


def read_member(archive, path, depth=0):
    """Reads a file from the archive, following symlinks inside it."""
    member = archive.getmember(path)
    if member.issym() and depth < 8:
        target = os.path.normpath(os.path.join(os.path.dirname(path), member.linkname))
        return read_member(archive, target, depth + 1)
    if not member.isfile():
        raise KeyError(path)
    return archive.extractfile(member).read()


def fetch(url, cache):
    if cache:
        os.makedirs(cache, exist_ok=True)
        cached = os.path.join(cache, re.sub(r"[^A-Za-z0-9._-]", "_", url))
        if not os.path.exists(cached):
            with urllib.request.urlopen(url) as response, open(cached + ".part", "wb") as out:
                shutil.copyfileobj(response, out)
            os.rename(cached + ".part", cached)
        return open(cached, "rb").read()
    with urllib.request.urlopen(url) as response:
        return response.read()


def build_theme(theme, cache, output):
    theme_id, display, url, release, roots, licenses, spdx = theme
    archive = tarfile.open(fileobj=io.BytesIO(fetch(url, cache)), mode="r:gz")
    prefix = archive.getnames()[0].split("/")[0] + "/"
    members = [name[len(prefix):] for name in archive.getnames() if name.startswith(prefix)]
    candidates = index_theme(members, roots)

    directory = os.path.join(output, theme_id)
    os.makedirs(directory)
    counts, missing, rejected = {}, [], []
    for context, icons, context_dirs in CONTEXTS:
        os.makedirs(os.path.join(directory, "scalable", context))
        counts[context] = 0
        for canonical, names in icons.items():
            for _, _, _, path in choose(candidates, names, context_dirs, context):
                try:
                    data = read_member(archive, prefix + path)
                    reason = check_svg(data)
                except (KeyError, UnicodeDecodeError, ElementTree.ParseError) as error:
                    reason = f"unreadable ({error.__class__.__name__})"
                if reason:
                    rejected.append(f"{path}: {reason}")
                    continue
                with open(os.path.join(directory, "scalable", context, canonical + ".svg"), "wb") as out:
                    out.write(data)
                counts[context] += 1
                break
            else:
                missing.append(canonical)

    for license_file in licenses:
        try:
            data = read_member(archive, prefix + license_file)
        except KeyError:
            continue
        with open(os.path.join(directory, os.path.basename(license_file)), "wb") as out:
            out.write(data)
    with open(os.path.join(directory, "index.theme"), "w") as out:
        out.write(f"[Icon Theme]\nName={display}\nComment=Subset of {display} {release} for OpsRemote\n"
                  f"Directories={','.join('scalable/' + c for c, _, _ in CONTEXTS)}\n\n")
        for context, _, _ in CONTEXTS:
            out.write(f"[scalable/{context}]\nSize=48\nMinSize=8\nMaxSize=512\nType=Scalable\n"
                      f"Context={context.capitalize()}\n\n")
    total = sum(counts.values())
    print(f"{display:13} {release:11} {total:3} icons "
          f"({', '.join(f'{c} {n}' for c, n in counts.items())}); "
          f"{len(missing)} not provided, {len(rejected)} candidates rejected")
    return display, release, url, spdx, total, missing, rejected


# Derived themes: (id, display name, base theme, tint). Coloured parts of the
# base theme's icons take the tint's hue and are softened (pale, matte);
# greys, black and white stay. The licence is the base theme's.
TINTED = [
    ("Papirus-Orange", "Papirus Orange", "Papirus", (0xE0, 0x95, 0x5A)),
]
HEX_COLOUR = re.compile(rb"#([0-9a-fA-F]{6}|[0-9a-fA-F]{3})\b")


def tint_colour(match, tint):
    digits = match.group(1).decode()
    if len(digits) == 3:
        digits = "".join(d * 2 for d in digits)
    r, g, b = (int(digits[i:i + 2], 16) / 255 for i in (0, 2, 4))
    hue, lightness, saturation = colorsys.rgb_to_hls(r, g, b)
    if saturation < 0.2 or lightness < 0.08 or lightness > 0.95:
        return match.group(0)
    # The tint's hue, spread a little by the original hue so icons stay
    # distinguishable; softer saturation, a touch lighter.
    tint_hue, _, _ = colorsys.rgb_to_hls(*(c / 255 for c in tint))
    hue = (tint_hue + (hue - 0.5) * 0.08) % 1.0
    r, g, b = colorsys.hls_to_rgb(hue, min(0.82, 0.12 + lightness * 0.95), 0.25 + saturation * 0.4)
    return ("#%02x%02x%02x" % tuple(round(c * 255) for c in (r, g, b))).encode()


def build_tinted(theme, output):
    theme_id, display, base, tint = theme
    directory = os.path.join(output, theme_id)
    shutil.copytree(os.path.join(output, base), directory)
    for folder, _, files in os.walk(directory):
        for name in files:
            path = os.path.join(folder, name)
            if name.endswith(".svg"):
                with open(path, "rb") as svg:
                    data = HEX_COLOUR.sub(lambda match: tint_colour(match, tint), svg.read())
                with open(path, "wb") as svg:
                    svg.write(data)
    index = os.path.join(directory, "index.theme")
    with open(index) as file:
        text = file.read()
    with open(index, "w") as file:
        file.write(re.sub(r"^Name=.*$", f"Name={display}", text, count=1, flags=re.M)
                   .replace(f"Comment=Subset of {base} ", f"Comment=Recoloured subset of {base} "))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cache", help="directory to keep downloaded archives in")
    arguments = parser.parse_args()

    staging = tempfile.mkdtemp(dir=os.path.dirname(TARGET) if os.path.isdir(os.path.dirname(TARGET)) else None)
    try:
        results = [build_theme(theme, arguments.cache, staging) for theme in THEMES]
        for theme in TINTED:
            build_tinted(theme, staging)
        with open(os.path.join(staging, "README.md"), "w") as out:
            out.write("# Icon themes\n\nSubsets of freedesktop icon themes: only the icons Portable "
                      "Terminal uses (actions, folders, and SFTP file types). Files are copied unmodified "
                      "from the pinned upstream releases below, after checking that each is a plain, single "
                      "SVG drawing. Refresh them with `scripts/internal/update-icon-themes.py`.\n\n"
                      "| Theme | Release | Icons | License | Source |\n|---|---|---|---|---|\n")
            for display, release, url, spdx, total, _, _ in results:
                out.write(f"| {display} | {release} | {total} | {spdx} | {url} |\n")
            for theme_id, display, base, _ in TINTED:
                out.write(f"| {display} | (from {base}) | | as {base} | {base}, recoloured by "
                          f"`update-icon-themes.py` |\n")
            out.write("\nEach theme's folder contains its upstream license files. Icons a theme does not "
                      "provide fall back to the application's built-in icons. Recoloured themes are "
                      "modified copies: their coloured parts take a pale, matte tint "
                      "(see `TINTED` in the script).\n")
        if os.path.isdir(TARGET):
            shutil.rmtree(TARGET)
        os.rename(staging, TARGET)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    print(f"Vendored {len(results)} icon themes into {TARGET}")


if __name__ == "__main__":
    sys.exit(main())
