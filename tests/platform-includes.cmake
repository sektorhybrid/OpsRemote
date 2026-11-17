# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
#
# Code that calls an operating system's own APIs belongs in src/platform (see
# docs/building.md, "Code for one operating system"). Fails when a file in
# src/app includes such a header and is not listed below with its reason.
#   cmake -DSOURCE=<project folder> -P tests/platform-includes.cmake

set(ALLOWED
    localresources.cpp   # X11 display and device checks, small per-system pieces
    main.cpp             # process start and the Windows busy-pointer diagnostic
    rdpoptions.cpp       # the Windows keyboard layout, one call
    sshauth.cpp          # finds a libssh function at run time, one call
    sshfiles.cpp         # timeval for libssh's SFTP times
    sshsession.cpp       # sockets for libssh (jump hosts, X11 and agent forwarding)
    stallwatch.cpp       # GUI-stall diagnostic, stack sampling for each system
    streamconnection.cpp # half-closing a socket
)
set(HEADERS "windows|wincred|tlhelp32|psapi|winsock2|ws2tcpip|shellapi|dbghelp|dlfcn|unistd|pwd|termios|signal|execinfo|sys/[a-z_]+")

file(GLOB FILES "${SOURCE}/src/app/*.cpp" "${SOURCE}/src/app/*.h")
set(PROBLEMS "")
foreach(FILE IN LISTS FILES)
    get_filename_component(NAME "${FILE}" NAME)
    if(NAME IN_LIST ALLOWED)
        continue()
    endif()
    file(STRINGS "${FILE}" LINES REGEX "^[ \t]*#[ \t]*include[ \t]*<(${HEADERS})\\.h>")
    foreach(LINE IN LISTS LINES)
        string(STRIP "${LINE}" LINE)
        string(APPEND PROBLEMS "\n  src/app/${NAME}: ${LINE}")
    endforeach()
endforeach()
if(PROBLEMS)
    message(FATAL_ERROR "Operating-system headers outside src/platform:${PROBLEMS}\n"
                        "Move that code to src/platform (docs/building.md), or add the file to "
                        "ALLOWED in tests/platform-includes.cmake with the reason.")
endif()
message(STATUS "src/app includes no operating-system headers outside the allowed files")
