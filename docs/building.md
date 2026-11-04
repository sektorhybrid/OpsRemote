# Building OpsRemote

Back to the [README](../README.md). Where settings are kept: [Portable data](portable-data.md).

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
| `package-zip` | Packs the current package as `dist/OpsRemote-<arch>-<systems>-<version>.tar.gz` (for example `OpsRemote-x86_64-linux-windows-0.5.0.tar.gz`) and `.zip` for handing out (the version is in `CMakeLists.txt`), without your settings and backups |
| `run-package` | Starts the package's Linux version |
| `package-linux-and-run` | `package-linux`, then `run-package` (the default) |
| `package-all-and-run` | `package-all`, then `run-package` |
| `build-qt-windows` | Rebuilds Qt's Windows DLLs with the patches in `packaging/qt/patches` (about half an hour). Only needed when those patches or the Windows image's Qt version change; the Windows build warns when it is |
| `dev-build`, `dev-run`, `dev-build-and-run` | A quick developer build in `./build/dev` against this computer's libraries (not portable) |

### Portable bundle

`./scripts/internal/make-bundle-docker.sh` builds a self-contained folder, `build/stage/OpsRemote-linux-<arch>/`, plus a `.tar.gz` of it, inside an AlmaLinux 9 container (Docker required). Qt, libssh, OpenSSL, yaml-cpp, libsodium, ICU, and the Qt plugins are bundled in `lib/`. Copy or extract it anywhere and run `./opsremote.sh`.

The target system needs to provide:

- glibc 2.34 or newer: RHEL/Alma/Rocky 9, Ubuntu 22.04+, Debian 12+, and current Fedora and Arch;
- an X11 or Wayland desktop;
- the system font and graphics libraries.

`./scripts/internal/make-bundle-windows-docker.sh` cross-compiles the Windows bundle, `build/stage/OpsRemote-windows-x86_64/`, plus a `.zip` of it, with MinGW-w64 in a Fedora container. Run `OpsRemote.exe` from the extracted folder on 64-bit Windows 10 (1809) or 11. Local terminals are not available on Windows yet; SSH sessions, SFTP, and the vault are.

`./scripts/internal/test-bundle-docker.sh` checks the result on Ubuntu 22.04, Debian 12, Fedora 42, AlmaLinux 9, and Arch. `./scripts/internal/make-bundle.sh` builds the same bundle directly on the host, but then it needs the host's glibc version.

Use `./scripts/build-and-run.sh --help` to see options. For example, set `BUILD_TYPE=Release` or `BUILD_DIR=/tmp/opsremote-build` before invoking the script to override the defaults.

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
| `dist/` | Only the release: `dist/OpsRemote/`, plus `OpsRemote-<arch>-<systems>-<version>.tar.gz` and `.zip` after `package-zip` |
