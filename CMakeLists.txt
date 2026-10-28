# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
cmake_minimum_required(VERSION 3.21)

project(OpsRemote VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# QTermWidget is linked with -Bsymbolic-functions, so the executable must be
# position independent: otherwise &QTermWidget::sendData in the executable is
# a different address from the one the library uses, and signal connections
# fail at runtime with "signal not found". Arch's GCC builds PIE by default,
# AlmaLinux 9's does not.
include(CheckPIESupported)
check_pie_supported()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network)
find_package(yaml-cpp REQUIRED)
# yaml-cpp before 0.8 exports its target without a namespace.
if(NOT TARGET yaml-cpp::yaml-cpp AND TARGET yaml-cpp)
    add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(LIBSSH REQUIRED IMPORTED_TARGET libssh>=0.10)
# Remote desktop (RDP) through FreeRDP 3's library and its software GDI.
pkg_check_modules(FREERDP REQUIRED IMPORTED_TARGET freerdp3>=3.5 freerdp-client3>=3.5 winpr3>=3.5)
# VNC through LibVNCServer's client library (UTF-8 clipboard and desktop
# resizing need 0.9.14 or newer).
pkg_check_modules(LIBVNCCLIENT REQUIRED IMPORTED_TARGET libvncclient>=0.9.14)

# QTermWidget is bundled so the portable build does not rely on a distribution package.
add_subdirectory(third_party/lxqt-build-tools EXCLUDE_FROM_ALL)
list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}")
# Cross toolchains (MinGW) search only their sysroot for packages; point
# QTermWidget at the build-tools config generated in this build directly.
set(lxqt2-build-tools_DIR "${CMAKE_BINARY_DIR}")
add_subdirectory(third_party/qtermwidget EXCLUDE_FROM_ALL)

qt_add_executable(opsremote
    src/app/main.cpp
    src/app/mainwindow.cpp
    src/app/mainwindow.h
    src/app/session.cpp
    src/app/session.h
    src/app/remotescreenpage.cpp
    src/app/remotescreenpage.h
    src/app/sessionendedpanel.cpp
    src/app/sessionendedpanel.h
    src/app/vncoptions.cpp
    src/app/vncoptions.h
    src/app/vncpage.cpp
    src/app/vncpage.h
    src/app/vncsession.cpp
    src/app/vncsession.h
    src/app/sshoptions.cpp
    src/app/sshoptions.h
    src/app/terminalpage.cpp
    src/app/terminalpage.h
    src/app/credentialvault.cpp
    src/app/credentialvault.h
    src/app/sshsession.cpp
    src/app/sshsession.h
    src/app/sftppanel.cpp
    src/app/sftppanel.h
    src/app/terminalappearance.cpp
    src/app/terminalappearance.h
    src/app/icons.cpp
    src/app/icons.h
    src/app/appdirs.cpp
    src/app/appdirs.h
    src/app/masterpassworddialog.cpp
    src/app/masterpassworddialog.h
    src/app/preferences.cpp
    src/app/preferences.h
    src/app/preferenceswindow.cpp
    src/app/preferenceswindow.h
    src/app/rdpclipboard.cpp
    src/app/rdpclipboard.h
    src/app/rdpoptions.cpp
    src/app/rdpoptions.h
    src/app/rdpsession.cpp
    src/app/rdpsession.h
    src/app/rdppage.cpp
    src/app/rdppage.h
)

# A GUI program on Windows: no console window behind it.
set_target_properties(opsremote PROPERTIES WIN32_EXECUTABLE ON)
target_compile_definitions(opsremote PRIVATE
    OPSREMOTE_VERSION="${PROJECT_VERSION}")

target_link_libraries(opsremote PRIVATE
    Qt6::Widgets
    Qt6::Network
    yaml-cpp::yaml-cpp
    PkgConfig::SODIUM
    PkgConfig::LIBSSH
    PkgConfig::FREERDP
    PkgConfig::LIBVNCCLIENT
    qtermwidget6
)

option(OPSREMOTE_BUNDLE
       "Find shared libraries in ./lib next to the executable (see scripts/make-bundle.sh)" OFF)
if(OPSREMOTE_BUNDLE)
    # An old-style DT_RPATH, not RUNPATH: glibc also searches the executable's
    # DT_RPATH for dependencies of bundled libraries that have no search path
    # of their own (libssh, ICU, ...). Bundled Qt libraries and plugins already
    # carry relative RUNPATHs that match the bundle's lib/ layout.
    set_target_properties(opsremote PROPERTIES
        BUILD_WITH_INSTALL_RPATH ON
        INSTALL_RPATH "$ORIGIN/lib")
    target_link_options(opsremote PRIVATE "LINKER:--disable-new-dtags")
    set_target_properties(qtermwidget6 PROPERTIES
        BUILD_WITH_INSTALL_RPATH ON
        INSTALL_RPATH "$ORIGIN")
endif()

add_custom_command(TARGET opsremote POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qtermwidget/lib/kb-layouts"
        "$<TARGET_FILE_DIR:opsremote>/data/qtermwidget/kb-layouts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qtermwidget/lib/color-schemes"
        "$<TARGET_FILE_DIR:opsremote>/data/qtermwidget/color-schemes"
    # Curated extra schemes (MIT, see third_party/iterm2-color-schemes).
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/iterm2-color-schemes"
        "$<TARGET_FILE_DIR:opsremote>/data/color-schemes"
    # Icon theme subsets (see third_party/icon-themes/README.md).
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/icon-themes"
        "$<TARGET_FILE_DIR:opsremote>/data/icons"
)

install(TARGETS opsremote RUNTIME DESTINATION bin)
