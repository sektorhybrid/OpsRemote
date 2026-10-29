# OpsRemote changes to QTermWidget

This copy of [QTermWidget](https://github.com/lxqt/qtermwidget) is based on
upstream commit `0a6d829649862c76904977675c60c3a609aa176e` (2026-10-18). It was
modified for OpsRemote by Thanos Athanasopoulos in 2026, as the licences
(GPL-2.0-or-later; LGPL-2.0-or-later for the library sources) require changed
files to state. The complete diff against upstream is in
[OPSREMOTE-CHANGES.patch](OPSREMOTE-CHANGES.patch).

| File | Change |
|---|---|
| `CMakeLists.txt` | Builds on Windows (MinGW): uses utf8proc for character widths, and leaves out the Unix pseudo-terminal sources. |
| `lib/Pty.h` | Selects the Windows `Pty` on Windows. |
| `lib/PtyWindows.h`, `lib/PtyWindows.cpp` | **New:** a `Pty` stand-in for Windows, where there are no Unix pseudo-terminals. © 2026 Thanos Athanasopoulos, LGPL-2.0-or-later. |
| `lib/History.h`, `lib/History.cpp`, `lib/BlockArray.cpp` | Scrollback history without `mmap()`/`getpagesize()` on Windows. |
| `lib/Session.cpp` | Ends a session without POSIX signals on Windows. |
| `lib/qtermwidget.h`, `lib/qtermwidget.cpp` | API additions: feed output from an external transport (SSH), a signal when the screen size changes, and `userTyped()`, emitted for typing also when a local program runs in a pseudo-terminal (for broadcast and multi-exec). |

Changed places in the sources are marked "OpsRemote addition".
