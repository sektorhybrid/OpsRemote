# Publishing a release

Back to the [README](../README.md). Licences: [THIRD_PARTY.md](../THIRD_PARTY.md).

GPLv3 section 6 requires the source of what you distribute to be available. For each release:

1. Commit everything and tag the release (for example `v0.1.0`).
2. Build the packages: `./scripts/build-and-run.sh package-all`, then `package-zip`.
3. Make the source archive: `./scripts/internal/make-source-archive.sh v0.1.0`. It writes `dist/OpsRemote-<version>-source.tar.xz`, which holds the tagged source and the exact upstream archives of the libraries built in Docker (checked against the Dockerfiles' SHA-256 sums).
4. Upload the package zip **and** the source archive to the GitHub release.

Libraries taken from AlmaLinux and Fedora packages are listed with their exact source packages in each package's `licenses/<platform>/DISTRIBUTION-PACKAGES.txt`. `SOURCES.txt` points to those distributions' source repositories and includes a three-year written offer as a fallback.
