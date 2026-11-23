# OpsRemote changes to LibVNCServer

OpsRemote builds libvncclient from the official LibVNCServer 0.9.15 release
(GPL-2.0-or-later; version and checksum in `packaging/linux/Dockerfile` and
`packaging/windows/Dockerfile`), with three security fixes from upstream's
own repository applied, because no release contains them yet. Modified for
OpsRemote by Thanos Athanasopoulos in 2026, as the GPL requires changed
files to state. The patches are unchanged upstream commits, in `patches/`.

| Patch | Upstream commit | Fixes |
|---|---|---|
| `0001-libvncclient-ultrazip-subrectangle-bounds.patch` | `009008e` (2026-03-22) | Heap out-of-bounds read in UltraZip decoding (GHSA-87q7-v983-qwcj) |
| `0002-libvncclient-tight-gradient-overflow.patch` | `5b27054` (2026-05-06) | Heap and stack out-of-bounds writes in Tight gradient decoding (CVE-2026-44988, GHSA-jcc5-8wj4-7c58) |
| `0003-libvncclient-tight-too-many-rows.patch` | `540332b` (2026-05-29) | Heap out-of-bounds write when Tight decompresses more rows than the rectangle has (CVE-2026-50538, GHSA-v9pm-47h4-jcq8) |

A malicious VNC server could trigger each of them. The files changed are
`src/libvncclient/ultra.c`, `src/libvncclient/tight.c` and
`include/rfb/rfbclient.h` (a named constant only). When a LibVNCServer
release includes these fixes, the patches are dropped.
