# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Cross-build environment for the portable Windows bundle: Fedora's MinGW-w64
# packages provide Qt and most dependencies as Windows (x86_64) libraries.
FROM fedora:42

RUN dnf -y install \
        cmake ninja-build make xz curl findutils file zip \
        mingw64-gcc-c++ mingw64-pkg-config mingw64-winpthreads-static \
        mingw64-qt6-qtbase mingw64-qt6-qttools mingw64-qt6-qtsvg mingw64-qt6-qtimageformats \
        mingw64-openssl mingw64-zlib mingw64-libsodium mingw64-yaml-cpp \
        qt6-qtbase-devel qt6-qttools-devel \
    && dnf clean all

# libssh is not packaged for MinGW. Build the pinned release from its
# official tarball (signature verified against the libssh release key
# 88A2 28D8 9B07 C2C7 7D0C 7809 03D5 DF8C FDD3 E8E7 when this pin was set).
ARG LIBSSH_VERSION=0.11.5
ARG LIBSSH_SHA256=6898ba9dd836d618b71dc7a4bb786a502c173cef5cafbf20fe5e0567ba4ea30c
RUN curl -fsSLo /tmp/libssh.tar.xz \
        "https://www.libssh.org/files/0.11/libssh-${LIBSSH_VERSION}.tar.xz" \
    && echo "${LIBSSH_SHA256}  /tmp/libssh.tar.xz" | sha256sum -c - \
    && mkdir /tmp/libssh && tar -C /tmp/libssh --strip-components=1 -xf /tmp/libssh.tar.xz \
    && mingw64-cmake -S /tmp/libssh -B /tmp/libssh/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DWITH_GSSAPI=OFF -DWITH_EXAMPLES=OFF \
        -DUNIT_TESTING=OFF -DCLIENT_TESTING=OFF -DWITH_SERVER=OFF \
    && cmake --build /tmp/libssh/build \
    && cmake --install /tmp/libssh/build \
    && rm -rf /tmp/libssh /tmp/libssh.tar.xz

# utf8proc (MIT) gives QTermWidget character widths; Windows has no wcwidth().
ARG UTF8PROC_VERSION=2.12.0
ARG UTF8PROC_SHA256=f564011d38b2888d583d510b08e69ffa15aa117155db1b9b49ef1dfe1fa25111
RUN curl -fsSLo /tmp/utf8proc.tar.gz \
        "https://github.com/JuliaStrings/utf8proc/archive/refs/tags/v${UTF8PROC_VERSION}.tar.gz" \
    && echo "${UTF8PROC_SHA256}  /tmp/utf8proc.tar.gz" | sha256sum -c - \
    && mkdir /tmp/utf8proc && tar -C /tmp/utf8proc --strip-components=1 -xf /tmp/utf8proc.tar.gz \
    && mingw64-cmake -S /tmp/utf8proc -B /tmp/utf8proc/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DUTF8PROC_ENABLE_TESTING=OFF \
    && cmake --build /tmp/utf8proc/build \
    && cmake --install /tmp/utf8proc/build \
    && rm -rf /tmp/utf8proc /tmp/utf8proc.tar.gz

# FreeRDP 3 (Apache-2.0) for remote desktop tabs: the core and client libraries
# with WinPR and the built-in channels, without client programs, servers, or
# bundled media codecs. H.264 for the graphics pipeline uses Windows' own
# Media Foundation decoder (mfplat.dll, loaded at run time; editions without
# it fall back to RemoteFX). The pinned
# release tarball was verified against the maintainer's signing key
# 7703 B333 420E 0AEF 995E B4B3 A494 54A3 FC90 9FD5 when this pin was set.
# MinGW's C library has no C11 <threads.h>; __STDC_NO_THREADS__ makes WinPR
# use __thread for its thread-local storage instead. FreeRDP 3.32.2 passes
# DWORD* for UINT32* in deprecated keyboard code; both are 32-bit on Windows,
# but GCC 14 rejects it unless that check is only a warning.
ARG FREERDP_VERSION=3.32.2
ARG FREERDP_SHA256=e95a505c0057e4d06a3b120477c421028769deba4db9c639ef3465d050f0955c
RUN curl -fsSLo /tmp/freerdp.tar.gz \
        "https://github.com/FreeRDP/FreeRDP/releases/download/${FREERDP_VERSION}/freerdp-${FREERDP_VERSION}.tar.gz" \
    && echo "${FREERDP_SHA256}  /tmp/freerdp.tar.gz" | sha256sum -c - \
    && mkdir /tmp/freerdp && tar -C /tmp/freerdp --strip-components=1 -xf /tmp/freerdp.tar.gz \
    && mingw64-cmake -S /tmp/freerdp -B /tmp/freerdp/build -G Ninja \
        -DCMAKE_C_FLAGS="-D__STDC_NO_THREADS__ -Wno-error=incompatible-pointer-types" \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DWITH_MANPAGES=OFF \
        -DWITH_CLIENT=OFF -DWITH_CLIENT_COMMON=ON -DWITH_CLIENT_SDL=OFF -DWITH_SERVER=OFF \
        -DWITH_SAMPLE=OFF -DWITH_PROXY=OFF -DWITH_SHADOW=OFF -DWITH_PLATFORM_SERVER=OFF \
        -DWITH_CHANNELS=ON -DWITH_CLIENT_CHANNELS=ON -DCHANNEL_URBDRC=OFF -DCHANNEL_TSMF=OFF \
        -DWITH_WINPR_TOOLS=OFF -DWITH_FFMPEG=OFF -DWITH_SWSCALE=OFF \
        -DWITH_DSP_FFMPEG=OFF -DWITH_CAIRO=OFF -DWITH_X11=OFF -DWITH_WAYLAND=OFF -DWITH_FUSE=OFF \
        -DWITH_PCSC=OFF -DWITH_SMARTCARD_EMULATE=OFF -DWITH_KRB5=OFF -DWITH_CUPS=OFF \
        -DWITH_PULSE=OFF -DWITH_ALSA=OFF -DWITH_OSS=OFF -DWITH_OPENH264=OFF -DWITH_WINMM=ON \
        -DWITH_JSON_DISABLED=ON -DWITH_AAD=OFF -DWITH_SSO_MIB=OFF -DWITH_WEBVIEW=OFF \
        -DWITH_URIPARSER=OFF -DWITH_LIBSYSTEMD=OFF -DUSE_UNWIND=OFF -DWITH_UNICODE_BUILTIN=ON \
        -DWITH_FDK_AAC=OFF -DWITH_LAME=OFF -DWITH_FAAD2=OFF -DWITH_FAAC=OFF -DWITH_SOXR=OFF \
        -DWITH_GSM=OFF -DWITH_OPUS=OFF -DWITH_MEDIA_FOUNDATION=ON \
    && cmake --build /tmp/freerdp/build \
    && cmake --install /tmp/freerdp/build \
    && rm -rf /tmp/freerdp /tmp/freerdp.tar.gz

# zstd (BSD) decompresses the packaged program at startup, so hosts need no
# zstd of their own. Built from the release tarball, verified against the
# Zstandard release key 4EF4 AC63 455F C9F4 545D 9B7D EF8F E995 28B5 2FFD
# when this pin was set.
ARG ZSTD_VERSION=1.5.7
ARG ZSTD_SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
RUN curl -fsSLo /tmp/zstd.tar.gz \
        "https://github.com/facebook/zstd/releases/download/v${ZSTD_VERSION}/zstd-${ZSTD_VERSION}.tar.gz" \
    && echo "${ZSTD_SHA256}  /tmp/zstd.tar.gz" | sha256sum -c - \
    && mkdir /tmp/zstd && tar -C /tmp/zstd --strip-components=1 -xf /tmp/zstd.tar.gz \
    && mingw64-cmake -S /tmp/zstd/build/cmake -B /tmp/zstd/out -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DZSTD_BUILD_PROGRAMS=ON -DZSTD_BUILD_SHARED=OFF \
        -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_TESTS=OFF -DZSTD_LEGACY_SUPPORT=OFF \
        -DZSTD_MULTITHREAD_SUPPORT=OFF -DZSTD_ZLIB_SUPPORT=OFF -DZSTD_LZMA_SUPPORT=OFF \
        -DZSTD_LZ4_SUPPORT=OFF \
        -DCMAKE_EXE_LINKER_FLAGS="-static"\
    && cmake --build /tmp/zstd/out --target zstd \
    && install -D -m 0755 /tmp/zstd/out/programs/zstd.exe /opt/portable-tools/zstd.exe \
    && rm -rf /tmp/zstd /tmp/zstd.tar.gz

# LibVNCServer's client library (GPL-2.0-or-later) for VNC tabs, with TLS
# (VeNCrypt and anonymous TLS) through OpenSSL and JPEG for the Tight
# encoding. The project publishes no signed release files; this pin is the
# SHA-256 of the GitHub release archive as downloaded when it was set.
ARG LIBVNC_VERSION=0.9.15
ARG LIBVNC_SHA256=62352c7795e231dfce044beb96156065a05a05c974e5de9e023d688d8ff675d7
RUN dnf -y install mingw64-libjpeg-turbo && dnf clean all \
    && curl -fsSLo /tmp/libvnc.tar.gz \
        "https://github.com/LibVNC/libvncserver/archive/refs/tags/LibVNCServer-${LIBVNC_VERSION}.tar.gz" \
    && echo "${LIBVNC_SHA256}  /tmp/libvnc.tar.gz" | sha256sum -c - \
    && mkdir /tmp/libvnc && tar -C /tmp/libvnc --strip-components=1 -xf /tmp/libvnc.tar.gz \
    && mingw64-cmake -S /tmp/libvnc -B /tmp/libvnc/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
        -DWITH_ZLIB=ON -DWITH_JPEG=ON -DWITH_OPENSSL=ON -DWITH_GNUTLS=OFF -DWITH_GCRYPT=OFF \
        -DWITH_LZO=OFF -DWITH_PNG=OFF -DWITH_SDL=OFF -DWITH_GTK=OFF -DWITH_QT=OFF \
        -DWITH_LIBSSHTUNNEL=OFF -DWITH_SYSTEMD=OFF -DWITH_FFMPEG=OFF -DWITH_SASL=OFF \
        -DWITH_XCB=OFF -DWITH_WEBSOCKETS=OFF -DWITH_TIGHTVNC_FILETRANSFER=OFF \
        -DWITH_EXAMPLES=OFF -DWITH_TESTS=OFF \
    && cmake --build /tmp/libvnc/build \
    && cmake --install /tmp/libvnc/build \
    && rm -rf /tmp/libvnc /tmp/libvnc.tar.gz

# Licence texts of the libraries built from source above, for the licenses/
# folder of the packages: taken from the same pinned, verified archives.
# cpio, bzip2, and xz unpack source packages for licence texts the image lacks
# (collect-licenses.sh).
RUN set -e; \
    dnf -y install cpio bzip2 xz && dnf clean all; \
    take() { \
        curl -fsSLo /tmp/lic.archive "$1"; \
        echo "$2  /tmp/lic.archive" | sha256sum -c -; \
        mkdir -p /tmp/lic "/opt/licenses/$3"; \
        tar -C /tmp/lic --strip-components=1 -xf /tmp/lic.archive; \
        find /tmp/lic -maxdepth 1 -type f \( -name 'LICENSE*' -o -name 'COPYING*' -o -name 'NOTICE*' \) \
            -exec cp {} "/opt/licenses/$3/" \; ; \
        rm -rf /tmp/lic /tmp/lic.archive; \
    }; \
    take "https://www.libssh.org/files/0.11/libssh-${LIBSSH_VERSION}.tar.xz" "${LIBSSH_SHA256}" libssh; \
    take "https://github.com/JuliaStrings/utf8proc/archive/refs/tags/v${UTF8PROC_VERSION}.tar.gz" "${UTF8PROC_SHA256}" utf8proc; \
    take "https://github.com/FreeRDP/FreeRDP/releases/download/${FREERDP_VERSION}/freerdp-${FREERDP_VERSION}.tar.gz" "${FREERDP_SHA256}" FreeRDP; \
    take "https://github.com/facebook/zstd/releases/download/v${ZSTD_VERSION}/zstd-${ZSTD_VERSION}.tar.gz" "${ZSTD_SHA256}" zstd; \
    take "https://github.com/LibVNC/libvncserver/archive/refs/tags/LibVNCServer-${LIBVNC_VERSION}.tar.gz" "${LIBVNC_SHA256}" libvncserver

# Qt Serial Port for serial console sessions (a separate step, so the
# layers above stay cached).
RUN dnf -y install mingw64-qt6-qtserialport && dnf clean all
