# Kvantum in OpsRemote

This is the style engine of [Kvantum](https://github.com/tsujan/Kvantum) 1.1.8
(released 2026-05-30), © Pedram Pourang (Tsu Jan) and contributors, under the
GNU General Public License version 3 or later (see [COPYING](COPYING)).
OpsRemote compiles it into the program instead of loading it as a Qt style
plugin, so it works the same in the portable Linux and Windows packages.

Only `Kvantum/style` (without the Qt 4 and Qt 5 variants, the build files and
the plugin entry point) and these themes from `Kvantum/themes/kvthemes` are
included: KvAdapta, KvAdaptaDark, KvArc, KvArcDark, KvFlat, KvFlatLight,
KvGnome, KvGnomeDark, KvMojave, KvMojaveLight, KvSimplicity, KvSimplicityDark,
KvYaru. KvMojaveLightOrange is made from KvMojaveLight by
`scripts/internal/make-kvantum-orange.py` (modified by Thanos Athanasopoulos,
2026): its blue accents and the selection colour are a pale orange. Kvantum Manager and Kvantum Preview are not included.

## Changes

Modified for OpsRemote by Thanos Athanasopoulos in 2026, as the GPL requires
changed files to state. The complete diff against the release is in
[OPSREMOTE-CHANGES.patch](OPSREMOTE-CHANGES.patch).

| File | Change |
|---|---|
| `style/Kvantum.cpp` | With `KVANTUM_CONFIG_HOME` defined, `kvantum.kvconfig` and the user's themes are read from that folder (OpsRemote's `settings/`) instead of `~/.config`. `kvantum.kvconfig` is read as an INI file (`QSettings::IniFormat`): `NativeFormat` means the registry on Windows, so no theme was ever read there. |
| `style/themeconfig/ThemeConfig.cpp` | Theme `.kvconfig` files are read as INI files (`QSettings::IniFormat`) instead of `NativeFormat`, which is the registry on Windows. With `KVANTUM_NO_X11` defined, X11 is not used and compositing is never assumed, so menus and tooltips are opaque, without translucency or shadows (translucent popups showed black areas where Qt gives them no alpha channel). |
| `style/blur/blurhelper.h`, `style/blur/blurhelper.cpp` | With `KVANTUM_NO_X11` and `NO_KF` defined, neither X11 nor KDE Frameworks is used, so there is no blur behind translucent menus. |
| `themes/KvMojaveLight/KvMojaveLight.kvconfig`, `KvMojaveLight.svg` | Tree expanders are small triangles: `[TreeExpander]` uses the `arrow` element (size 9) instead of `sarrow` (large plus and minus signs), and the SVG gains `arrow-plus-*` and `arrow-minus-*` (Kvantum draws `<element>-plus`/`-minus` for expanders) as references to `arrow-right-*` and `arrow-down-*`. |
| `opsremote-paths.h` | **New:** declares the two functions OpsRemote provides for the folders (`settings/` and `data/`). © 2026 Thanos Athanasopoulos, GPL-3.0-or-later. |

The build (OpsRemote's `CMakeLists.txt`) defines `NO_KF`, `KVANTUM_NO_X11`,
`KVANTUM_CONFIG_HOME=opsremoteKvantumConfigHome()` and
`DATADIR=opsremoteKvantumDataDir()`, so the bundled themes are found in
`data/Kvantum/` and the user's own in `settings/Kvantum/`.
