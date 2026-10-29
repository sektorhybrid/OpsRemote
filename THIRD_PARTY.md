# Third-party components

OpsRemote is © 2026 Thanos Athanasopoulos and licensed under the
[GNU General Public License, version 3 or later](LICENSE). It uses the
components below, each under its own licence. All of them are compatible with
GPL-3.0-or-later.

## In this repository

These are kept in `third_party/` and built into OpsRemote or shipped with it.

| Component | Version | Licence | Authors | Notes |
|---|---|---|---|---|
| [QTermWidget](https://github.com/lxqt/qtermwidget) | commit `0a6d829` | GPL-2.0-or-later; some files LGPL-2.0-or-later or BSD-3-Clause | Konsole and QTermWidget authors ([AUTHORS](third_party/qtermwidget/AUTHORS)) | **Modified** for OpsRemote by Thanos Athanasopoulos (2026): Windows support and an external-transport API. See [OPSREMOTE-CHANGES.md](third_party/qtermwidget/OPSREMOTE-CHANGES.md) and the full diff in [OPSREMOTE-CHANGES.patch](third_party/qtermwidget/OPSREMOTE-CHANGES.patch). |
| [lxqt-build-tools](https://github.com/lxqt/lxqt-build-tools) | commit `4d1ab7d` | BSD-3-Clause | LXQt team ([AUTHORS](third_party/lxqt-build-tools/AUTHORS)) | Used only to build QTermWidget; not shipped. Unmodified. |
| [iTerm2-Color-Schemes](https://github.com/mbadolato/iTerm2-Color-Schemes) | commit `27cbbb5` | MIT | Mark Badolato and contributors | 75 schemes in Konsole format, unmodified ([LICENSE](third_party/iterm2-color-schemes/LICENSE)). |
| [Kvantum](https://github.com/tsujan/Kvantum) style engine and 13 of its themes | 1.1.8 | GPL-3.0-or-later | Pedram Pourang (Tsu Jan) and contributors | **Modified** for OpsRemote by Thanos Athanasopoulos (2026): compiled in instead of loaded as a plugin, builds without X11 and KDE Frameworks, reads its folders from OpsRemote. See [OPSREMOTE-CHANGES.md](third_party/kvantum/OPSREMOTE-CHANGES.md) and the full diff in [OPSREMOTE-CHANGES.patch](third_party/kvantum/OPSREMOTE-CHANGES.patch). |
| Icon themes: Breeze, Papirus, Papirus Dark, Adwaita, Yaru, Tela, Fluent, Colloid, Qogir, WhiteSur, elementary, Papirus Orange | see [the list](third_party/icon-themes/README.md) | LGPL-3.0-or-later, GPL-3.0, CC-BY-SA (per theme) | Each theme's authors (files in each theme folder) | Subsets of unmodified icons, except Papirus Orange: a copy of the Papirus subset recoloured to a pale orange by `scripts/internal/update-icon-themes.py` (modified by Thanos Athanasopoulos, 2026; same licence as Papirus). Each folder keeps its upstream licence and author files; `index.theme` is generated for the subset. |

The OpsRemote application icon (`packaging/icons/opsremote.svg` and the files
made from it) is original artwork by Thanos Athanasopoulos, under the same
GPL-3.0-or-later licence as the program.

## In the release packages

The packages also include these libraries, dynamically linked and unmodified.
They are built from the pinned sources in `packaging/*/Dockerfile`, or taken
from the AlmaLinux 9 (Linux) and Fedora MinGW (Windows) packages. Every
package carries the full licence texts in `licenses/<platform>/`.

| Component | Licence | Use |
|---|---|---|
| [Qt 6](https://www.qt.io) (Base, SVG, Serial Port, Wayland, image formats) | LGPL-3.0 | User interface |
| [FreeRDP](https://www.freerdp.com) 3.32.2 | Apache-2.0 | Remote desktop (RDP) |
| [LibVNCServer](https://libvnc.github.io)'s libvncclient 0.9.15 | GPL-2.0-or-later | VNC |
| [libssh](https://www.libssh.org) | LGPL-2.1-or-later | SSH and SFTP |
| [OpenSSL](https://www.openssl.org) 3 | Apache-2.0 | TLS and cryptography |
| [libsodium](https://libsodium.org) | ISC | Credential vault encryption |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | MIT | Settings files |
| [zstd](https://facebook.github.io/zstd/) 1.5.7 | BSD-3-Clause | Unpacking the program at startup |
| [zlib](https://zlib.net) | Zlib | Compression |
| [libjpeg-turbo](https://libjpeg-turbo.org) | IJG, BSD-3-Clause, Zlib | VNC JPEG decoding |
| [utf8proc](https://github.com/JuliaStrings/utf8proc) (Windows) | MIT | Character widths in the terminal |
| [ICU](https://icu.unicode.org) (Linux) | Unicode-3.0 | Text handling for Qt |
| GCC runtime and MinGW-w64 libraries (Windows) | GPL-3.0 with the GCC Runtime Library Exception; MIT and ZPL-2.1 | C/C++ runtime |
| Further dependencies of the above (for example FreeType, HarfBuzz, libpng, PCRE2, GLib, D-Bus, xkbcommon) | Various open-source licences | See `licenses/<platform>/` in the package |

## Source code

- **OpsRemote:** the complete source, including the modified QTermWidget, is
  in this repository. Each release is built from a tagged commit.
- **Libraries built from source** (FreeRDP, LibVNCServer, zstd; on Windows
  also libssh and utf8proc): the Dockerfiles in `packaging/` list the exact
  upstream archives with their SHA-256 checksums, and each release's source
  archive (`OpsRemote-<version>-source.tar.xz`, made by
  `scripts/internal/make-source-archive.sh`) contains them.
- **Libraries from distribution packages** (Qt, OpenSSL and the rest): each
  package lists them with their exact source packages in
  `licenses/<platform>/DISTRIBUTION-PACKAGES.txt`. Their sources are published
  by AlmaLinux and Fedora. `SOURCES.txt` in the package also carries a written
  offer to provide any of these sources for at least three years.
- **Replacing libraries:** Qt and the other LGPL libraries are separate shared
  libraries in the package (`lib/` on Linux, next to `OpsRemote.exe` on
  Windows), so you can replace them with your own builds.
