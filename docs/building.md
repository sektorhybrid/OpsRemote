# Building OpsRemote

Back to the [README](../README.md). Licences: [THIRD_PARTY.md](../THIRD_PARTY.md). Where settings are kept: [Portable data](portable-data.md).

## What you need

- Linux with **Docker**. The packages are built in containers, so they run on many Linux distributions and on Windows.
- For a quick local build only: CMake 3.21+, a C++20 compiler, Qt 6 Widgets (6.4+), yaml-cpp, libsodium and libssh 0.12+.

## Build

```sh
./scripts/build-and-run.sh package-all         # Linux and Windows, into dist/OpsRemote/
./scripts/build-and-run.sh run-package         # start the Linux version
./scripts/build-and-run.sh dev-build-and-run   # a quick local build in build/dev (not portable)
```

`./scripts/build-and-run.sh --help` lists every command.

The result, `dist/OpsRemote/`, is one folder with both programs and one shared `settings/` folder. It runs on:
- 64-bit Linux with glibc 2.34 or newer: RHEL, Alma and Rocky 9, Ubuntu 22.04+, Debian 12+, and current Fedora and Arch;
- 64-bit Windows 10 (1809) or 11.

## Libraries and their sources

Some libraries are built from their official releases inside the build images. Their versions and checksums are in `packaging/linux/Dockerfile` and `packaging/windows/Dockerfile`. Where OpsRemote changes another project's code, the change is listed in that project's `third_party/<name>/OPSREMOTE-CHANGES.md` (with the full diff) or in `packaging/qt/patches/` (Qt for Windows), `packaging/qt-linux/` (Qt for Linux) and `packaging/libvncserver/` (LibVNC). Rebuilding Qt (`./scripts/build-and-run.sh build-qt-windows` or `build-qt-linux`) is only needed after changing those patches or the build images' Qt version; the package build warns when it is. Each release also has a source archive with all of these sources.
