# Publishing a release

Back to the [README](../README.md). Licences: [THIRD_PARTY.md](../THIRD_PARTY.md).

For the interactive workflow, run `./scripts/release.sh <version>` (for example,
`./scripts/release.sh 0.6.2`). It prepares the commit and tag without pushing,
builds both platforms and the archives, then asks for review before pushing
`main` and the tag. After the release dry run, confirm to enter the existing
draft/publish/stop prompt. Answering no stops and preserves completed work;
the displayed undo command is for manual use before pushing only. Run from
`main`, with GitHub CLI authentication and the build dependencies installed.
Include all intended changes in the release commit: the workflow requires a
clean working tree so the built packages and tagged source agree.

GPLv3 section 6 requires the source of what you distribute to be available. For each release:

1. While working, write each change under `## Next` at the top of [What's new](changes.md), one `- **Title:** text` line each; this section becomes the release notes.
2. Raise the version in `project(OpsRemote VERSION …)` in `CMakeLists.txt` (after each big batch of features and fixes), rename `## Next` in What's new to it, commit everything and tag the release (for example `v0.6.2`).
3. Build the packages: `./scripts/build-and-run.sh package-all`, then `package-zip`.
4. Make the source archive: `./scripts/internal/make-source-archive.sh v0.6.2`. It writes `dist/OpsRemote-<version>-source.tar.xz`, which holds the tagged source and the exact upstream archives of the libraries built in Docker (checked against the Dockerfiles' SHA-256 sums).
5. Upload `dist/OpsRemote-<arch>-<systems>-<version>.tar.gz` and `.zip` (for example `OpsRemote-x86_64-linux-windows-0.6.2.tar.gz`), their checksums `OpsRemote-<arch>-<systems>-<version>.sha256`, **and** the source archive to the GitHub release. Downloads are checked with `sha256sum -c --ignore-missing OpsRemote-…-<version>.sha256` (on Windows: `Get-FileHash <file>` and compare).

Libraries taken from AlmaLinux and Fedora packages are listed with their exact source packages in each package's `licenses/<platform>/DISTRIBUTION-PACKAGES.txt`. `SOURCES.txt` points to those distributions' source repositories and includes a three-year written offer as a fallback.
