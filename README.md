# OpsRemote

Early C++/Qt Widgets prototype of OpsRemote, a portable terminal and remote-access workspace for Windows and Linux: local terminals, SSH with SFTP, remote desktop (RDP), and VNC in one program that runs from a folder or USB stick.

Created by **Thanos Athanasopoulos** ([@sektorhybrid](https://github.com/sektorhybrid)). Free software under the [GNU GPL, version 3 or later](LICENSE).

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

In the package, each program is a `.tar.zst` archive in the `compressed/` folder, which shrinks the package from about 185 MB to 63 MB. The launcher unpacks it once per version into the `runtime/` folder next to itself (visible, not hidden, so the unpacking is not mistaken for something to conceal; packages before 2026-10-28 used a hidden `.runtime/`, which the launcher removes) and reuses it; `settings/` stays outside. In the package, start `OpsRemote` on Linux or `OpsRemote.exe` on Windows (the scripts in `legacy/` do the same and are kept as fallbacks). Both check the system first, and both use the same `settings/` (sessions, vault, known hosts, and preferences).


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
  - **Looks:** **View → Look** switches between ready-made looks (OpsRemote light/dark/automatic, Nord, Dracula, Gruvbox, Solarized, Classic, the Kvantum looks Arc, Arc Dark, Mojave, Mojave Orange and Adapta Dark, and Windows 11 on Windows) that set colours, style, icons, fonts and terminal colours together. Save up to 20 of your own in **Preferences → Appearance → Looks**. The default **OpsRemote** style is a rounder, flatter take on Fusion. Fourteen [Kvantum](https://github.com/tsujan/Kvantum) themes are built in as styles; drop more Kvantum themes into `settings/Kvantum/`.
  - **Icon themes:** **Preferences → Appearance → Interface** switches the toolbar, menu, session-tree, and Files file-type icons between Qt's built-in icons and eleven bundled themes (Breeze, Papirus, Papirus Dark, Adwaita, Yaru, Tela, Fluent, Colloid, Qogir, WhiteSur, and elementary).
  - **Remote desktop (RDP):** RDP sessions open in a tab, with the remote desktop resizing along with the tab (or a fixed size, scaled to fit). Resolution, colour depth, connection quality, security, and administrator session have defaults in Preferences → Remote desktop and can be overridden per session. Keyboard and mouse go to the remote computer, certificates are confirmed once and remembered in `settings/rdp/`, and the password comes from the vault or a log-on prompt. Copy and paste of text and files works in both directions, and files can be dropped onto the remote screen (then pasted there with Ctrl+V); files copied on the remote computer are saved from a bar in the tab. The desktop uses the RDP 8 graphics pipeline (RemoteFX, and H.264 on Windows through its own Media Foundation decoder) and is sharp on high-DPI screens: it gets the screen's physical resolution and Windows scales to match. **Ctrl+Alt+Enter** (or **View → Remote Desktop Full Screen**) shows the desktop full screen, on all monitors if the session's options say so. An **RD Gateway** can be set per session or as a default in Preferences. Other options: the remote keyboard layout (this computer's by default), automatic reconnecting after a network drop, starting a program instead of the desktop, and logging on at the remote computer's own screen instead of being asked first (for xrdp and older servers). For a domain account, write the user name as `DOMAIN\user`. **Local resources** (Preferences → Remote desktop, or per session): remote audio (play here, leave on the remote computer, or off), the microphone, drives (none, the package's `shared/` folder, the home folder, all drives, or chosen folders, shown as `\\tsclient`), printers, and smart cards. Sound and printers are off unless turned on. On Linux, sound goes through the PulseAudio interface (PulseAudio or PipeWire), printing through CUPS, and smart cards through pcscd.
  - **Preferences** (`Ctrl+,`): Appearance, Terminal behaviour (cursor, scrollback, copy on select, right-click paste, bell, tab activity), Startup and tabs (reopen last tabs, confirmations, tab titles), Keyboard shortcuts (every shortcut can be changed), SSH and connections (keepalive, auto-reconnect, keys, trusted host keys), Files and transfers, Security and vault (auto-lock, change master password), Session logging, Snippets and broadcast, Highlight rules, Presentation mode, Portable data and backup, and About. Changes apply immediately.
  - **Interface font:** **Settings → Preferences… → Interface font** changes the font and size of the menus, panels, and dialogs. Terminals keep their own font.

**VNC** sessions open in a tab like remote desktops: the screen resizes with the tab where the server allows it (TigerVNC, recent x11vnc), otherwise it's scaled to fit; keyboard (any layout), mouse, the remote pointer shape, and clipboard text both ways; colour and quality presets; TLS (prefer or require); an a **jump host** (see below); `host:1` meaning port 5901; listen mode for reverse connections; and UltraVNC repeaters. Defaults are in **Preferences → Connections → VNC**.

Remote desktop and VNC tabs share **full screen** (Ctrl+Alt+Enter), and their tab menu has **Send Keys** (Ctrl+Alt+Del, Ctrl+Alt+Backspace, Ctrl+Esc, Alt+Tab, the Windows key, Print Screen) and **Save Screenshot…**. Exiting lists every session that is still connected.

When an SSH or remote desktop session ends or can't connect, a bar at the top of its tab says why and offers **Reconnect** (Enter) and **Close Tab** (Esc).

The session window shows the selected protocol's settings in tabs: **Appearance** and **Connection** for SSH, and **Display**, **Local resources**, and **Connection** for RDP and VNC. Every setting starts at "Default (…)", the Preferences value, and can be overridden per session; for SSH that covers the connect timeout, keepalive, compression, key logon, a private key file, automatic reconnecting, and the terminal type.

**Jump hosts (bastions):** SSH, RDP and VNC sessions can connect through one or more SSH servers first. Choose one on the session's **Jump host** tab, either a saved SSH session or `user@host[:port]`. A folder can set a default for all its sessions (right-click → **Folder Settings…**). A saved SSH session used as a jump host can have its own jump host, for several hops. Host keys and RDP certificates are remembered under the real server's name.

**Port forwarding and SSH tunnels:** six kinds of forward: **Local** (`-L`), **Remote** (`-R`), **SOCKS** (`-D`), **Reverse SOCKS** (`-R port`, a proxy on the server that goes out through this computer), **Socket** (a port here reaches a Unix socket on the server, such as Docker's), and an **HTTP proxy** for programs without SOCKS. An SSH session's **Port forwarding** tab opens them with the session. **Sessions → SSH Tunnels…** keeps tunnels of their own: a list with each tunnel's live state, connections, traffic and time running, Start All / Stop All, a filter, and **Import from ~/.ssh/config** (LocalForward, RemoteForward, DynamicForward, ProxyJump). The selected tunnel is shown as a diagram you edit in place (this computer → jump hosts → SSH server → destination, arrows turned round for remote kinds), with each hop's state, a sentence saying what it does, the matching `ssh` command to copy, **Open** for web pages, and **Run Checks** (is the port here free, does the server answer, does it reach the destination). A tunnel may carry several forwards over one connection (listed beside the diagram, with + Add and Remove), ride on an open tab's connection instead of logging on again (taking over by itself when the tab closes), wake its server with Wake-on-LAN and wait for it, and reconnect when the network drops. Forwarded ports accept only this computer unless you allow other computers; the diagram then lists the addresses they use.

Wherever you pick one of your saved SSH sessions (a jump host in the session and folder windows, the server of a tunnel, the server to install a key on in **SSH Keys**), the list ends with **New…**: it opens the New Session window over the tool you are in, with SSH chosen, and picks the new session once you save it.

**More SSH:** agent forwarding (`-A`, off by default); an HTTP or SOCKS5 proxy per session, per folder or for everything (**Preferences → Connections → Proxy**); and **Tools → SSH Keys…**, which lists, creates (optionally with a passphrase, which can be remembered in the vault), imports and deletes keys in `settings/keys/`, and installs the selected key on a server like `ssh-copy-id`. Keys with a passphrase are asked for when logging on. On Windows, agent forwarding works with the OpenSSH agent or PuTTY's Pageant.

**Local servers:** **Tools → Local Servers (TFTP, HTTP)…** serves files to network equipment (firmware, configurations): a TFTP server (with large blocks for speed) and an HTTP server (downloads, folder listings, and uploads by browser or `curl -T`). Each has its port (69 and 80 by default), where it listens (all network interfaces, this computer only, or one address), its folder (the package's `shared/` folder or another), read-only or with uploads, and the largest upload it accepts; the window remembers them. Uploads are written aside and put in place only when complete; the servers limit clients, sizes, and idle time. The servers are for the moment: they run only while the window is open, which shows this computer's addresses and a log of transfers. On Linux, ports below 1024 need extra rights; the window says how, or pick a higher port.

**Network tools:** **Tools → Network Tools…** has ping, traceroute, a TCP port scanner, a **bandwidth test** that speaks iperf3 (against any `iperf3 -s`, upload or download, parallel streams, with a graph; or as the server that `iperf3 -c` tests against, listening where you choose and refusing tests longer than the limit you set, with no iperf3 program needed here), and a **Download** tab (http, https, ftp, ftps, or ftpes into Downloads, the package's `shared/` folder for Local Servers, or another folder; resume; optionally ignore certificate errors for lab equipment). The tools start with the selected session's host.

**SCP and FTP:** the file browser works over SFTP, **SCP** (for routers, switches, and appliances without SFTP), or **FTP/FTPS**. SSH sessions choose under **File transfer**: Auto (SFTP, else SCP), SFTP only, or SCP only (Preferences → SSH → File browser, or per session). **SFTP** and **SCP** are also session types of their own (quick connect `sftp://`, `scp://`): the file browser fills the tab, with no terminal, using the session's SSH settings, keys, jump hosts, proxy, and port forwards. **FTP** is a session type of its own (quick connect `ftp://`, `ftps://` for implicit TLS, `ftpes://` for explicit TLS): TLS when the server offers it by default, or required, implicit, or none; passive or active mode; MLSD or LIST; a proxy; and a protocol log. Its tab is the same file browser, with Sync Folders, editing, and the same transfer questions. Defaults are in **Preferences → Connections → FTP**.

**Safe uploads:** files above 100 KB are uploaded as `name.filepart` and replace the old file only when complete, so a dropped connection never leaves a half-written file (**Preferences → Files and transfers → Uploads**: above a size, all files, or off). An FTP server without TLS is not sent a password until you agree (once, or always for that server).

**Sync folders:** the Files panel's **Sync Folders…** (toolbar or right-click) makes a server folder like a local one or the other way round, copying only new or changed files (size and modification time) and optionally deleting files only on the target, like rsync but over the session's own SFTP connection: no rsync needed on either side. A preview lists every change first.

**Wake-on-LAN:** a session's Wake-on-LAN tab holds its MAC address. Right-click → **Wake Up**, or let OpsRemote wake the computer before connecting.

**Session list:** every session has a small status dot: filled (pale green) when its host answers on its port, empty when not, blinking grey while it is checked, and a dash for sessions behind a jump host or proxy (OpsRemote only knocks on the port; it never logs on). Folders show a summary such as `3 ● 1 ○` (filled: answers; empty: does not). **Pinned** (right-click → Pin) and **Recent** sit at the top like other folders. The **search box** above the list (Sessions → Search Sessions, Ctrl+Shift+S) shows only the sessions whose name, host, user, protocol, or folder contain every word typed; Down goes to the results, Enter opens the first one, Esc clears. **Preferences → Appearance → Session list** turns the search box, Pinned, Recent, the Host and Protocol columns, and the status checks on or off, puts folder summaries after the name, in the next column, or nowhere, and lets the side panel always grow and shrink to fit the list.

**Split view:** **Tabs → Split Right** (Ctrl+Shift+E) or **Split Down** (Ctrl+Shift+O) opens the same session beside the active pane at once; the terminal's and the tab's right-click menus have **Split Right ▸ / Split Down ▸** with Same Session, Local Terminal, or Other Session… (a chooser; also Tabs → Split Right With…). Split as often as wanted and drag the dividers. The active pane has a thin accent frame; menus, snippets, the Files pane, and Duplicate Tab act on it, and Ctrl+Shift+W closes it (the tab, when it is not split). Remote desktops stay one per tab.

**HTTP Request:** **Tools → Network Tools → HTTP Request** sends any request (GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS or another method) with your headers and body (text, JSON, form or a file) and shows the answer: status, headers, body (JSON formatted, gzip and deflate decoded, binary as hexadecimal, **Save Body…**), the time of each step (name lookup, connect, TLS, waiting, receiving), and the connection and certificate (TLS version, cipher, the whole chain, whether it is trusted). Where the connection goes (**Connect to**), the name sent in the TLS handshake (**SNI**) and the **Host header** can each differ from the URL's host, for testing a server behind a load balancer or not yet in DNS, or which certificate a server gives for a name. **Repeat** sends the request many times, several at once, and shows the results and timing statistics (fastest, average, median, p90, p99, slowest). Requests can be saved by name, and **Copy as curl** gives the equivalent curl command. Defaults are in **Preferences → Connections → HTTP requests**.

**Run on Hosts:** right-click a folder → **Run Command on Hosts…** (or **Tools → Run Command on Hosts…** for the selected folder, or every session when none is selected) runs one command on the folder's SSH sessions, subfolders included, a few at a time over connections of their own, with their jump hosts, proxies and saved passwords. A table shows each host's result (✓ done, ✕ failed with its exit status, or why it could not connect), time and first line of output; choose a host to see all of it. Check the hosts to run on; **Run Again Where It Failed**, **Copy Results** and **Save as CSV** help with the rest. Each window sets how many hosts run at once, a time limit and whether to use a terminal (pty); the defaults, the output kept per host and the recent commands are in **Preferences → Connections → Run on hosts**. **Recent and Snippets** fills in an earlier command or a snippet.

**Multi-exec:** **Tabs → Multi-exec…** (Ctrl+Shift+M) picks terminals (tabs and split panes) to type into together; the tab menu and the terminal's right-click menu toggle one. Chosen terminals have an accent bar on top, and the status bar shows **MULTI-EXEC · N** (click it to stop). Typing into a chosen terminal goes to all chosen ones. Broadcast (Ctrl+Shift+B) still sends to every terminal.

**Password storage:** passwords can be kept in OpsRemote's encrypted vault (portable, behind the master password), in the system credential store (Windows Credential Manager, or the Linux keyring through libsecret: GNOME Keyring, KWallet, KeePassXC; no master password, but it stays on that computer), or in both. Choose in **Preferences → Security and vault → Password storage**, and per session with "Keep the password in" in the session window; changing a session's store moves its saved password. The same tab copies all saved passwords from the vault to the system store or back.

**Quick connect:** type `user@host`, `ssh://user@host:2222`, `telnet://switch`, `mosh://me@laptop`, `rdp://DOMAIN\user@desktop`, `vnc://host:1` or `serial://COM3:9600` into the box on the toolbar and press Enter. Without a protocol it is SSH. Recent targets are kept in `settings/quick-connect.yaml`.

**Command line:** `OpsRemote --session "Name"` (or `--session "Folder/Sub/Name"`) opens a saved session, and any quick-connect target opens one-off sessions, for example `OpsRemote ssh://root@server telnet://switch`. With OpsRemote already running, the open window opens them.

**Local terminals on Windows:** **File → New Local Terminal** opens Windows PowerShell in a pseudo console (ConPTY), like Windows Terminal does; **Preferences → Terminal → Local shell** switches to PowerShell 7, Command Prompt, WSL, or any other program (on Linux: your login shell or another program). Local terminals work in split panes, logging, recordings, and broadcast like any other terminal. **File → New Local Terminal (Administrator)** (also in the split pane chooser) runs the shell as administrator in a tab, after the Windows UAC prompt; a standard user types an administrator's name and password, and the shell then runs as that administrator. When an administrator has turned on Windows 11's sudo in inline mode, it is used instead (Preferences → Terminal → Local shell).

**Telnet, serial and Mosh:** Telnet and serial consoles (COM ports, `/dev/ttyUSB0`; speed, data bits, parity, stop bits and flow control on the session's **Serial line** tab) open in terminal tabs, with the usual colours, logging, recording and broadcast. Mosh sessions run the system's `mosh` client (`mosh` on this computer, `mosh-server` on the server); on Windows a `mosh` on the PATH (from Cygwin or MSYS2) is used.

**Rlogin, RSH and XDMCP:** for older Unix systems and equipment. **Rlogin** opens a terminal tab (port 513) with the window size kept up to date; **RSH** (port 514) runs the session's command and shows its output, and lines you type go to it (Ctrl+D ends the input, Ctrl+C stops). Both send your user name on this computer (or the one set as **Local user name**) and the session's user name; the server's `.rhosts` decides whether a password is asked. Most servers accept them only from a reserved source port (below 1024): Windows allows that to everyone, Linux and macOS only to an administrator, so **Source port** tries one and otherwise says why the server may refuse. Neither is encrypted. **XDMCP** shows a Unix host's graphical login (its own, its chooser, or the first to answer a broadcast) in a window of a local X server program: Xephyr or Xnest on Linux, VcXsrv or Xming on Windows (not included; the session window says when none is found, with **Check again**). The host connects back to this computer on TCP port 6000 + the display number (6010 and up), which the firewall must allow. Defaults are in **Preferences → Connections → Rlogin and RSH** and **XDMCP**; each session can override them.

**X11 forwarding:** turn it on in **Preferences → SSH → Keys** or per session; graphical programs started on the server show on this computer (`ssh -Y`). Windows needs an X server such as VcXsrv or Xming.

**Recordings:** right-click a terminal → **Start Recording** saves it as an asciinema `.cast` in the log folder (or record every session: **Preferences → Session logging → Recordings**). **Tools → Play Recording…** plays one back with pause, speed and a position slider; long pauses are shortened.

**Import sessions:** **Sessions → Import From** reads `~/.ssh/config`, PuTTY, MobaXterm (`.mxtsessions` or `MobaXterm.ini`), mRemoteNG (`confCons.xml`) and Remmina. Each import goes into its own folder; passwords are not imported.

**Edit remote files:** in the Files panel, **Edit** (or double-click) opens a file in your editor (Preferences → Files and transfers → Editor, or the system's), and each save uploads it again.

Start the package with **OpsRemote.exe** (Windows) or **OpsRemote** (Linux); `legacy/OpsRemote.bat` and `legacy/OpsRemote.sh` remain as fallbacks. The launchers check the system, unpack the program on first start, and report it if OpsRemote crashes.

Only one copy of OpsRemote runs at a time; starting it again brings the open window forward (Preferences → Workspace → Startup). On Linux with Wayland, OpsRemote does not ask the desktop to bring its windows forward unless **Bring windows to the front (Wayland)** is on: GNOME shows its busy pointer for several seconds after each such request, which Qt makes after most menus and dialogs. With it off, starting OpsRemote again does not raise the open window there.

Press **F2** on a session or folder to rename it in place, and **Del** to delete it. Sessions can also sit outside any folder. Right-click → **Duplicate** (or **Sessions → Duplicate**) copies a session (with its saved password, when the vault is open) into the same folder as `Name (1)`, `Name (2)`, and so on.

Sessions and folders in the Sessions tree can be dragged to reorder them or move them into another folder, within the three-level folder limit.

The **Files** tab in the left panel browses the remote files of the active SSH tab (over SFTP or SCP; its header says which). It switches with the tab and, after each command, shows the folder the shell is in (**Preferences → SSH → File browser**, or per session on its **Connection** tab). The right-click menus of the terminal (**Files Follow This Terminal's Folder**) and of the Files pane (**Follow the Terminal's Folder**) switch following for that tab only, without changing the session's or the global setting. Shells that report their folder (OSC 7) are followed directly; otherwise OpsRemote looks the folder up on the server without typing anything into the shell. That needs a Linux server that accepts `LC_*` variables, which most do:

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
| `third_party/` | Bundled QTermWidget, Kvantum, colour schemes, and icon themes |
| `packaging/` | Docker images and launchers |
| `scripts/` | `build-and-run.sh`; helpers in `scripts/internal/` |
| `build/` | Build trees, all disposable: `dev/` (developer build), `linux/` and `windows/` (package builds), `linux-host/`, `asan/`, and `stage/` (the per-platform bundles and tools the package is assembled from) |
| `dist/` | Only the release: `dist/OpsRemote/`, plus `OpsRemote.zip` after `package-zip` |

## Licence

OpsRemote is © 2026 Thanos Athanasopoulos and is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License or (at your option) any later version. It comes with ABSOLUTELY NO WARRANTY, to the extent permitted by law. See [LICENSE](LICENSE).

It includes and bundles third-party components under their own compatible licences, including modified copies of QTermWidget and Kvantum. See [THIRD_PARTY.md](THIRD_PARTY.md). Release packages carry every component's licence text in `licenses/`, and `SOURCES.txt` says where the source of everything in them is.

### Publishing a release

GPLv3 section 6 requires the source of what you distribute to be available. For each release:

1. Commit everything and tag the release (for example `v0.1.0`).
2. Build the packages: `./scripts/build-and-run.sh package-all`, then `package-zip`.
3. Make the source archive: `./scripts/internal/make-source-archive.sh v0.1.0`. It writes `dist/OpsRemote-<version>-source.tar.xz`, which holds the tagged source and the exact upstream archives of the libraries built in Docker (checked against the Dockerfiles' SHA-256 sums).
4. Upload the package zip **and** the source archive to the GitHub release.

Libraries taken from AlmaLinux and Fedora packages are listed with their exact source packages in each package's `licenses/<platform>/DISTRIBUTION-PACKAGES.txt`. `SOURCES.txt` points to those distributions' source repositories and includes a three-year written offer as a fallback.
