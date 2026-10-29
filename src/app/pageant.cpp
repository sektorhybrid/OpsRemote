// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "pageant.h"

#ifdef _WIN32

#include <QString>

#include <windows.h>

#include <atomic>
#include <cstring>
#include <memory>

namespace {

// From PuTTY's windows/agent-client.c.
constexpr ULONG_PTR AgentCopyDataId = 0x804e50ba;
constexpr DWORD AgentMaxMessage = 8192;

HWND pageantWindow()
{
    return FindWindowW(L"Pageant", L"Pageant");
}

// The current user's SID, which Pageant requires as the owner of the shared
// memory (so other users cannot hand it requests).
std::unique_ptr<BYTE[]> currentUserSid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    auto buffer = std::make_unique<BYTE[]>(size);
    const bool ok = GetTokenInformation(token, TokenUser, buffer.get(), size, &size);
    CloseHandle(token);
    if (!ok)
        return {};
    const PSID sid = reinterpret_cast<TOKEN_USER *>(buffer.get())->User.Sid;
    const DWORD length = GetLengthSid(sid);
    auto copy = std::make_unique<BYTE[]>(length);
    if (!CopySid(length, copy.get(), sid))
        return {};
    return copy;
}

}

bool Pageant::isRunning()
{
    return pageantWindow() != nullptr;
}

QByteArray Pageant::query(const QByteArray &request)
{
    const HWND window = pageantWindow();
    if (!window || request.size() < 4 || DWORD(request.size()) > AgentMaxMessage)
        return {};
    auto sid = currentUserSid();
    SECURITY_DESCRIPTOR descriptor;
    if (!sid || !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)
        || !SetSecurityDescriptorOwner(&descriptor, sid.get(), FALSE))
        return {};
    SECURITY_ATTRIBUTES attributes{sizeof attributes, &descriptor, FALSE};

    static std::atomic<unsigned> counter{0};
    const QByteArray name = QStringLiteral("PageantRequest%1%2")
                                .arg(GetCurrentThreadId(), 8, 16, QLatin1Char('0'))
                                .arg(counter++, 4, 16, QLatin1Char('0'))
                                .toLatin1();
    const HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0,
                                              AgentMaxMessage, name.constData());
    if (!mapping)
        return {};
    auto *memory = static_cast<unsigned char *>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0));
    if (!memory) {
        CloseHandle(mapping);
        return {};
    }
    std::memcpy(memory, request.constData(), size_t(request.size()));
    COPYDATASTRUCT data{AgentCopyDataId, DWORD(name.size() + 1),
                        const_cast<char *>(name.constData())};
    QByteArray reply;
    if (SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)) != 0) {
        const DWORD length = (DWORD(memory[0]) << 24) | (DWORD(memory[1]) << 16)
                             | (DWORD(memory[2]) << 8) | DWORD(memory[3]);
        if (length + 4 <= AgentMaxMessage)
            reply = QByteArray(reinterpret_cast<const char *>(memory), int(length + 4));
    }
    UnmapViewOfFile(memory);
    CloseHandle(mapping);
    return reply;
}

#else

bool Pageant::isRunning()
{
    return false;
}

QByteArray Pageant::query(const QByteArray &)
{
    return {};
}

#endif
