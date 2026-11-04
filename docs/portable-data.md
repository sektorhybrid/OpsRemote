# Portable data

Back to the [README](../README.md).

All user state (sessions, preferences, the vault, and known hosts) is kept in a `settings/` folder next to the executable. The bundle's `settings/` survives rebuilds. On its first run the bundle starts from a copy of `build/dev/settings/`, if that exists. Files that older versions kept in `data/` are moved into `settings/` automatically.

In the package, each program is a `.tar.zst` archive in the `compressed/` folder, which shrinks the package from about 185 MB to 63 MB. The launcher unpacks it once per version into the `runtime/` folder next to itself (visible, not hidden, so the unpacking is not mistaken for something to conceal; packages before 2026-10-28 used a hidden `.runtime/`, which the launcher removes) and reuses it; `settings/` stays outside. In the package, start `OpsRemote` on Linux or `OpsRemote.exe` on Windows (the scripts in `legacy/` do the same and are kept as fallbacks). Both check the system first, and both use the same `settings/` (sessions, vault, known hosts, and preferences).

Sessions are stored in `settings/sessions.yaml`, using a compact recursive `folder` / `sessions` YAML structure. UI preferences are stored in `settings/settings.yaml`, encrypted credentials in `settings/credentials.vault`, and trusted SSH host keys in `settings/known_hosts`, next to the running executable. Existing version-1 session YAML and `settings/sessions.json` files are imported into the current format on first launch; JSON files are left in place as a backup. Exported YAML never includes passwords. Portable data-directory selection and Windows packaging will be addressed in later steps.
