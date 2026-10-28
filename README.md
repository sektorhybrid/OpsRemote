# OpsRemote

Early C++/Qt Widgets prototype of OpsRemote, a portable terminal and remote-access workspace for Windows and Linux: local terminals, SSH with SFTP, remote desktop (RDP), and VNC in one program that runs from a folder or USB stick.

Created by **Thanos Athanasopoulos** ([@sektorhybrid](https://github.com/sektorhybrid)). Free software under the [GNU GPL, version 3 or later](LICENSE).

See [decisions and implementation so far](design/decisions-and-implementation.md) for the current architecture, configuration formats, security design, terminal integration, and known limitations.

## Build and run on Linux

Requirements: CMake 3.21+, a C++20 compiler, Qt 6 Widgets (6.4+), yaml-cpp, libsodium, and libssh 0.10+ (Arch: `libssh`; Debian/Ubuntu: `libssh-dev`; Fedora: `libssh-devel`).

The helper script builds the portable package `dist/OpsRemote/`, with Linux and Windows in one folder and one shared `settings/` folder (Docker is required), then launches it:

```sh
./scripts/build-and-run.sh
```

The commands say what is built and what is run:

| Command | What it does |
|---|---|
| `package-linux` | Builds the Linux program and updates the package (keeps the last Windows build) |
| `package-windows` | Builds the Windows program and updates the package (keeps the last Linux build) |
| `package-all` | Builds both programs and updates the package |
| `package-zip` | Zips the current package as `dist/OpsRemote.zip` for handing out, without your settings and backups |
| `run-package` | Starts the package's Linux version |
| `package-linux-and-run` | `package-linux`, then `run-package` (the default) |
| `package-all-and-run` | `package-all`, then `run-package` |
| `dev-build`, `dev-run`, `dev-build-and-run` | A quick developer build in `./build/dev` against this computer's libraries (not portable) |

All user state (sessions, preferences, the vault, and known hosts) is kept in a `settings/` folder next to the executable. The bundle's `settings/` survives rebuilds. On its first run the bundle starts from a copy of `build/dev/settings/`, if that exists. Files that older versions kept in `data/` are moved into `settings/` automatically.

In the package, each program is a `.tar.zst` archive, which shrinks the package from about 185 MB to 63 MB. The launcher unpacks it once per version into the `runtime/` folder next to itself (visible, not hidden, so the unpacking is not mistaken for something to conceal; packages before 2026-10-28 used a hidden `.runtime/`, which the launcher removes) and reuses it; `settings/` stays outside. In the package, start `OpsRemote.sh` on Linux or `OpsRemote.bat` on Windows. Both check the system first, and both use the same `settings/` (sessions, vault, known hosts, and preferences).


### Portable bundle

`./scripts/internal/make-bundle-docker.sh` builds a self-contained folder, `build/stage/OpsRemote-linux-<arch>/`, plus a `.tar.gz` of it, inside an AlmaLinux 9 container (Docker required). Qt, libssh, OpenSSL, yaml-cpp, libsodium, ICU, and the Qt plugins are bundled in `lib/`. Copy or extract it anywhere and run `./opsremote.sh`.

The target system needs to provide:

- glibc 2.34 or newer: RHEL/Alma/Rocky 9, Ubuntu 22.04+, Debian 12+, and current Fedora and Arch;
- an X11 or Wayland desktop;
- the system font and graphics libraries.

`./scripts/internal/make-bundle-windows-docker.sh` cross-compiles the Windows bundle, `build/stage/OpsRemote-windows-x86_64/`, plus a `.zip` of it, with MinGW-w64 in a Fedora container. Run `OpsRemote.exe` from the extracted folder on 64-bit Windows 10 (1809) or 11. Local terminals are not available on Windows yet; SSH sessions, SFTP, and the vault are.

`./scripts/internal/test-bundle-docker.sh` checks the result on Ubuntu 22.04, Debian 12, Fedora 42, AlmaLinux 9, and Arch. `./scripts/internal/make-bundle.sh` builds the same bundle directly on the host, but then it needs the host's glibc version.

Use `./scripts/build-and-run.sh --help` to see options. For example, set `BUILD_TYPE=Release` or `BUILD_DIR=/tmp/opsremote-build` before invoking the script to override the defaults.

The current prototype provides the main window, a three-level example session tree, folder/session creation, YAML session persistence and import/export, live UI style and color theme selectors saved in YAML, and a bundled QTermWidget local Bash terminal from **File → New Local Terminal**. Its terminal context menu provides copy, paste, search, clear, output export, zoom, and URL actions; `Ctrl++` / `Ctrl+-` zoom the terminal and `Ctrl+0` resets it. It also includes a libsodium-backed credential vault using an Argon2id-derived key and XChaCha20-Poly1305 authenticated encryption.

Opening a saved SSH session starts a live remote shell in its tab. The connection uses libssh on a background thread, with these behaviors:

- **Host-key checks:** fingerprints are verified against `settings/known_hosts`.
- **Authentication:** ssh-agent and default-key public-key login, the vault password, and keyboard-interactive or password prompts shown inside the terminal.
- **Terminal:** live window resizing.
- **Reconnect:** press Enter after a disconnect.
- **Themes:** set a default color scheme and font in **Settings → Preferences…**, or override them per SSH session in the session editor. `Ctrl`+mouse wheel also zooms.
  - **Color schemes:** the 14 original QTermWidget schemes are listed first, then 75 curated iTerm2-Color-Schemes themes (MIT). **Import…** adds your own `.colorscheme` files to `settings/color-schemes/`.
  - **Application color themes:** **View → Color Theme** also offers Nord, Solarized Dark/Light, Dracula, Gruvbox Dark, and High Contrast.
  - **Icon themes:** **View → Icon Theme** switches the toolbar, menu, session-tree, and SFTP file-type icons between Qt's built-in icons and eleven bundled themes (Breeze, Papirus, Papirus Dark, Adwaita, Yaru, Tela, Fluent, Colloid, Qogir, WhiteSur, and elementary).
  - **Remote desktop (RDP):** RDP sessions open in a tab, with the remote desktop resizing along with the tab (or a fixed size, scaled to fit). Resolution, colour depth, connection quality, security, and administrator session have defaults in Preferences → Remote desktop and can be overridden per session. Keyboard and mouse go to the remote computer, certificates are confirmed once and remembered in `settings/rdp/`, and the password comes from the vault or a log-on prompt. Copy and paste of text and files works in both directions, and files can be dropped onto the remote screen (then pasted there with Ctrl+V); files copied on the remote computer are saved from a bar in the tab. The desktop uses the RDP 8 graphics pipeline (RemoteFX, and H.264 on Windows through its own Media Foundation decoder) and is sharp on high-DPI screens: it gets the screen's physical resolution and Windows scales to match. **Ctrl+Alt+Enter** (or **View → Remote Desktop Full Screen**) shows the desktop full screen, on all monitors if the session's options say so. An **RD Gateway** can be set per session or as a default in Preferences. Other options: the remote keyboard layout (this computer's by default), automatic reconnecting after a network drop, starting a program instead of the desktop, and logging on at the remote computer's own screen instead of being asked first (for xrdp and older servers). For a domain account, write the user name as `DOMAIN\user`. Audio is not supported yet.
  - **Preferences** (`Ctrl+,`): Appearance, Terminal behaviour (cursor, scrollback, copy on select, right-click paste, bell, tab activity), Startup and tabs (reopen last tabs, confirmations, tab titles), Keyboard shortcuts (every shortcut can be changed), SSH and connections (keepalive, auto-reconnect, keys, trusted host keys), SFTP and transfers, Security and vault (auto-lock, change master password), Session logging, Snippets and broadcast, Highlight rules, Presentation mode, Portable data and backup, and About. Changes apply immediately.
  - **Interface font:** **Settings → Preferences… → Interface font** changes the font and size of the menus, panels, and dialogs. Terminals keep their own font.

**VNC** sessions open in a tab like remote desktops: the screen resizes with the tab where the server allows it (TigerVNC, recent x11vnc), otherwise it's scaled to fit; keyboard (any layout), mouse, the remote pointer shape, and clipboard text both ways; colour and quality presets; TLS (prefer or require); an **SSH tunnel** through a saved SSH session or `user@host`; `host:1` meaning port 5901; listen mode for reverse connections; and UltraVNC repeaters. Defaults are in **Preferences → Connections → VNC**.

Remote desktop and VNC tabs share **full screen** (Ctrl+Alt+Enter), and their tab menu has **Send Keys** (Ctrl+Alt+Del, Ctrl+Alt+Backspace, Ctrl+Esc, Alt+Tab, the Windows key, Print Screen) and **Save Screenshot…**. Exiting lists every session that is still connected.

When an SSH or remote desktop session ends or can't connect, a bar at the top of its tab says why and offers **Reconnect** (Enter) and **Close Tab** (Esc).

The session window shows the selected protocol's settings in tabs: **Appearance** and **Connection** for SSH, and **Display**, **Local resources**, and **Connection** for RDP and VNC. Every setting starts at "Default (…)", the Preferences value, and can be overridden per session; for SSH that covers the connect timeout, keepalive, compression, key logon, a private key file, automatic reconnecting, and the terminal type.

Press **F2** on a session or folder to rename it in place. Right-click → **Duplicate** (or **Sessions → Duplicate**) copies a session (with its saved password, when the vault is open) into the same folder as `Name (1)`, `Name (2)`, and so on.

Sessions and folders in the Sessions tree can be dragged to reorder them or move them into another folder, within the three-level folder limit.

The **SFTP** tab in the left panel browses the remote files of the active SSH tab. It switches with the tab and doesn't follow the shell's folder:

- **Uploading:** copy files or folders in your file manager and press Ctrl+V or right-click → Paste in the browser, or drop them onto it.
- **Context menu:** Download…, Upload Files/Folder…, New Folder…, Rename…, Delete…, Copy Path, and Refresh.
- **Transfers:** a progress bar with a cancel button appears at the bottom of the pane. Existing files prompt Keep Both / Overwrite / Skip.



Sessions are stored in `settings/sessions.yaml`, using a compact recursive `folder` / `sessions` YAML structure. UI preferences are stored in `settings/settings.yaml`, encrypted credentials in `settings/credentials.vault`, and trusted SSH host keys in `settings/known_hosts`, next to the running executable. Existing version-1 session YAML and `settings/sessions.json` files are imported into the current format on first launch; JSON files are left in place as a backup. Exported YAML never includes passwords. Portable data-directory selection and Windows packaging will be addressed in later steps.

## Scripts

`scripts/build-and-run.sh` is the only script you need: it builds (Linux, Windows, or both) and runs the package. The steps it calls, and maintenance helpers (bundle and package assembly, Docker wrappers, the distribution test matrix, colour scheme and icon theme updates, the test VM deploy), live in `scripts/internal/`.

## Project layout

| Folder | Contents |
|---|---|
| `src/` | Application source |
| `third_party/` | Bundled QTermWidget, colour schemes, and icon themes |
| `packaging/` | Docker images and launchers |
| `scripts/` | `build-and-run.sh`; helpers in `scripts/internal/` |
| `design/` | Design notes and the [programming safety review](design/programming-safety-review.md) |
| `build/` | Build trees, all disposable: `dev/` (developer build), `linux/` and `windows/` (package builds), `linux-host/`, `asan/`, and `stage/` (the per-platform bundles and tools the package is assembled from) |
| `dist/` | Only the release: `dist/OpsRemote/`, plus `OpsRemote.zip` after `package-zip` |

## Licence

OpsRemote is © 2026 Thanos Athanasopoulos and is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License or (at your option) any later version. See [LICENSE](LICENSE).

It includes and bundles third-party components under their own compatible licences, including a modified copy of QTermWidget. See [THIRD_PARTY.md](THIRD_PARTY.md). Release packages carry every component's licence text in `licenses/`.
