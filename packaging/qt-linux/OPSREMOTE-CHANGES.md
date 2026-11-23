# OpsRemote changes to Qt base (Linux)

The Linux package takes Qt from AlmaLinux 9 (`qt6-qtbase` 6.10.3, LGPL-3.0).
AlmaLinux has no update with Qt's 2026 security fixes, and Qt released no
fixed 6.10 version, so OpsRemote rebuilds AlmaLinux's own source package
with Qt's published fixes added. Only the two libraries they change,
`libQt6Core.so.6` and `libQt6Network.so.6`, replace AlmaLinux's in the
package. Modified for OpsRemote by Thanos Athanasopoulos in 2026, as the
licence requires changed files to state. The patches are in `patches/`.

| Patch | From | Fixes |
|---|---|---|
| `0001-CVE-2026-76151-network-cache-control.patch` | Qt's 6.10 diff, unchanged | Over-read of a `Cache-Control` header from a web server (CVE-2026-76151) |
| `0002-CVE-2026-78253-xmlstream.patch` | Qt's 6.10 diff, unchanged | Stack exhaustion in `QXmlStreamReader` (CVE-2026-78253) |
| `0003-CVE-2026-16762-xmlstream-1.patch`, `0004-CVE-2026-16762-xmlstream-2.patch` | Qt's 6.11 diffs, unchanged | CPU exhaustion in `QXmlStreamReader` (CVE-2026-16762) |
| `0005-CVE-2026-105946-ntlm-1.patch`, `0007-CVE-2026-105946-ntlm-3.patch` | Qt's 6.11 diffs, unchanged | Over-read in NTLM authentication (CVE-2026-105946) |
| `0006-CVE-2026-105946-ntlm-2.patch` | Qt's 6.11 diff, backported | The same; Qt's test changes left out (they don't match 6.10 and are not built) |
| `0008-CVE-2026-95620-http2-upload-device.patch` | Qt's 6.11 diff, backported | Use-after-free in HTTP/2 uploads (CVE-2026-95620); the same two lines, placed by hand because a line next to them differs in 6.10 |

Qt's advisories: <https://www.qt.io/blog/security-advisory-cve-2026-76151>,
<https://www.qt.io/blog/security-advisory-cve-2026-78253>,
<https://www.qt.io/blog/cve-2026-16762>,
<https://www.qt.io/blog/security-advisory-cve-2026-105946>,
<https://www.qt.io/blog/security-advisory-cve-2026-95620>.
The rebuilt Qt Core uses Qt's built-in copies of double-conversion
(BSD-3-Clause) and BLAKE2 (CC0-1.0 or Apache-2.0) where AlmaLinux's links
the system's `libdouble-conversion` and `libb2`, the same projects; their
licence texts are in this package's `licenses/` folder.

The patches are dropped when AlmaLinux ships a Qt that contains them.
