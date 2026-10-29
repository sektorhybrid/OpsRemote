// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "rdpsession.h"
#include "appdirs.h"
#include "localresources.h"
#include "rdpclipboard.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QMetaObject>
#include <QTimer>

#include <freerdp/addin.h>
#include <freerdp/client.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/channels/rdpdr.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client/disp.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/codec/h264.h>
#include <freerdp/codec/color.h>
#include <freerdp/event.h>
#include <freerdp/graphics.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/input.h>
#include <freerdp/settings.h>
#include <winpr/synch.h>

#include <chrono>
#include <condition_variable>
#include <vector>
#include <cstring>
#include <mutex>
#include <memory>

namespace {
// FreeRDP allocates ContextSize bytes for the context; ours carries the owner.
struct PortableRdpContext
{
    rdpContext base;
    RdpSession *owner;
};

// Windows' display scaling for a device pixel ratio: the desktop scale is a
// percentage (100-500); the device scale is one of 100, 140, or 180.
UINT32 desktopScale(qreal ratio)
{
    return static_cast<UINT32>(qBound(100, qRound(ratio * 100), 500));
}

UINT32 deviceScale(qreal ratio)
{
    return ratio >= 1.6 ? 180 : ratio >= 1.2 ? 140 : 100;
}

// Whether this FreeRDP build has an H.264 decoder that works here.
bool h264Available()
{
    static const bool available = [] {
        H264_CONTEXT *h264 = h264_context_new(FALSE);
        h264_context_free(h264);
        return h264 != nullptr;
    }();
    return available;
}

RdpSession *ownerOf(rdpContext *context)
{
    return context ? reinterpret_cast<PortableRdpContext *>(context)->owner : nullptr;
}

QString text(const char *value)
{
    return value ? QString::fromUtf8(value) : QString();
}

// Replaces an allocated FreeRDP string argument.
void replaceString(char **target, const QString &value)
{
    free(*target);
    *target = _strdup(value.toUtf8().constData());
}
}

// FreeRDP allocates pointer.size bytes per cached pointer shape; ours keeps
// the decoded image.
struct PortableRdpPointer
{
    rdpPointer base;
    QImage *image;
};

// FreeRDP callbacks; they run on the worker thread.
struct RdpCallbacks
{
    static BOOL pointerNew(rdpContext *context, rdpPointer *pointer)
    {
        auto *portable = reinterpret_cast<PortableRdpPointer *>(pointer);
        auto image = std::make_unique<QImage>(static_cast<int>(pointer->width),
                                              static_cast<int>(pointer->height),
                                              QImage::Format_ARGB32);
        if (image->isNull())
            return FALSE;
        // BGRA32 in memory is QImage::Format_ARGB32 on little-endian machines.
        if (!freerdp_image_copy_from_pointer_data(
                image->bits(), PIXEL_FORMAT_BGRA32, static_cast<UINT32>(image->bytesPerLine()), 0,
                0, pointer->width, pointer->height, pointer->xorMaskData, pointer->lengthXorMask,
                pointer->andMaskData, pointer->lengthAndMask, pointer->xorBpp,
                &context->gdi->palette))
            return FALSE;
        portable->image = image.release();
        return TRUE;
    }

    static void pointerFree(rdpContext *, rdpPointer *pointer)
    {
        auto *portable = reinterpret_cast<PortableRdpPointer *>(pointer);
        delete portable->image;
        portable->image = nullptr;
    }

    static BOOL pointerSet(rdpContext *context, rdpPointer *pointer)
    {
        auto *portable = reinterpret_cast<PortableRdpPointer *>(pointer);
        if (!portable->image)
            return TRUE;
        auto *owner = ownerOf(context);
        const QImage shape = portable->image->copy();
        const QPoint hotSpot(static_cast<int>(pointer->xPos), static_cast<int>(pointer->yPos));
        QMetaObject::invokeMethod(owner, [owner, shape, hotSpot] {
            emit owner->pointerChanged(shape, hotSpot);
        }, Qt::QueuedConnection);
        return TRUE;
    }

    static BOOL pointerSetNull(rdpContext *context)
    {
        auto *owner = ownerOf(context);
        QMetaObject::invokeMethod(owner, [owner] { emit owner->pointerHidden(); },
                                  Qt::QueuedConnection);
        return TRUE;
    }

    static BOOL pointerSetDefault(rdpContext *context)
    {
        auto *owner = ownerOf(context);
        QMetaObject::invokeMethod(owner, [owner] { emit owner->pointerDefault(); },
                                  Qt::QueuedConnection);
        return TRUE;
    }

    static BOOL pointerSetPosition(rdpContext *, UINT32, UINT32)
    {
        // The local mouse stays where the user put it.
        return TRUE;
    }

    static BOOL preConnect(freerdp *instance)
    {
        // Channels report themselves once the server opened them.
        PubSub_SubscribeChannelConnected(instance->context->pubSub, channelConnected);
        PubSub_SubscribeChannelDisconnected(instance->context->pubSub, channelDisconnected);
        return TRUE;
    }

    static void channelConnected(void *context, const ChannelConnectedEventArgs *event)
    {
        auto *owner = ownerOf(static_cast<rdpContext *>(context));
        if (std::strcmp(event->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
            if (owner->m_clipboard)
                owner->m_clipboard->attach(static_cast<CliprdrClientContext *>(event->pInterface));
            return;
        }
        if (std::strcmp(event->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
            gdi_graphics_pipeline_init(static_cast<rdpContext *>(context)->gdi,
                                       static_cast<RdpgfxClientContext *>(event->pInterface));
            return;
        }
        if (std::strcmp(event->name, DISP_DVC_CHANNEL_NAME) != 0)
            return;
        auto *display = static_cast<DispClientContext *>(event->pInterface);
        display->custom = owner;
        display->DisplayControlCaps = displayCaps;
        owner->m_display = display;
    }

    static void channelDisconnected(void *context, const ChannelDisconnectedEventArgs *event)
    {
        auto *owner = ownerOf(static_cast<rdpContext *>(context));
        if (std::strcmp(event->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
            if (owner->m_clipboard)
                owner->m_clipboard->attach(nullptr);
            return;
        }
        if (std::strcmp(event->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
            gdi_graphics_pipeline_uninit(static_cast<rdpContext *>(context)->gdi,
                                         static_cast<RdpgfxClientContext *>(event->pInterface));
            return;
        }
        if (std::strcmp(event->name, DISP_DVC_CHANNEL_NAME) != 0)
            return;
        owner->m_display = nullptr;
        owner->m_displayReady = false;
    }

    // The server supports resizing; size requests can be sent from now on.
    static UINT displayCaps(DispClientContext *display, UINT32 maxMonitors, UINT32, UINT32)
    {
        auto *owner = static_cast<RdpSession *>(display->custom);
        owner->m_maxMonitors = qBound<UINT32>(1, maxMonitors, 16);
        owner->m_displayReady = true;
        QMetaObject::invokeMethod(owner, [owner] { emit owner->resizeSupported(); },
                                  Qt::QueuedConnection);
        return CHANNEL_RC_OK;
    }

    static BOOL postConnect(freerdp *instance)
    {
        if (!gdi_init(instance, PIXEL_FORMAT_BGRX32))
            return FALSE;
        auto *context = instance->context;
        context->update->EndPaint = endPaint;
        context->update->DesktopResize = desktopResize;
        rdpPointer pointer{};
        pointer.size = sizeof(PortableRdpPointer);
        pointer.New = pointerNew;
        pointer.Free = pointerFree;
        pointer.Set = pointerSet;
        pointer.SetNull = pointerSetNull;
        pointer.SetDefault = pointerSetDefault;
        pointer.SetPosition = pointerSetPosition;
        graphics_register_pointer(context->graphics, &pointer);
        auto *owner = ownerOf(context);
        const auto *gdi = context->gdi;
        owner->resizeFrame(static_cast<int>(gdi->width), static_cast<int>(gdi->height));
        const int width = static_cast<int>(gdi->width);
        const int height = static_cast<int>(gdi->height);
        QMetaObject::invokeMethod(owner, [owner, width, height] {
            emit owner->connected(width, height);
        }, Qt::QueuedConnection);
        return TRUE;
    }

    static void postDisconnect(freerdp *instance)
    {
        gdi_free(instance);
    }

    static BOOL endPaint(rdpContext *context)
    {
        auto *gdi = context->gdi;
        if (!gdi || !gdi->primary || !gdi->primary->hdc)
            return TRUE;
        auto *window = gdi->primary->hdc->hwnd;
        if (!window || !window->invalid || window->invalid->null)
            return TRUE;
        const QRect area(window->invalid->x, window->invalid->y, window->invalid->w,
                         window->invalid->h);
        ownerOf(context)->resizeFrame(static_cast<int>(gdi->width), static_cast<int>(gdi->height));
        ownerOf(context)->copyFrame(gdi->primary_buffer, static_cast<int>(gdi->stride),
                                    static_cast<int>(gdi->width), static_cast<int>(gdi->height),
                                    area);
        window->invalid->null = TRUE;
        window->ninvalid = 0;
        return TRUE;
    }

    static BOOL desktopResize(rdpContext *context)
    {
        auto *settings = context->settings;
        const auto width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
        const auto height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
        if (!gdi_resize(context->gdi, width, height))
            return FALSE;
        auto *owner = ownerOf(context);
        owner->resizeFrame(static_cast<int>(width), static_cast<int>(height));
        QMetaObject::invokeMethod(owner, [owner, width, height] {
            emit owner->desktopResized(static_cast<int>(width), static_cast<int>(height));
        }, Qt::QueuedConnection);
        return TRUE;
    }

    static DWORD verifyCertificate(freerdp *instance, const char *host, UINT16 port,
                                   const char *commonName, const char *subject,
                                   const char *issuer, const char *fingerprint, DWORD)
    {
        Q_UNUSED(commonName);
        if (auto *owner = ownerOf(instance->context); !owner->m_certificateName.isEmpty())
            return owner->verifyTunnelledCertificate(text(subject), text(issuer), text(fingerprint));
        const auto decision = ownerOf(instance->context)->askCertificate(
            QStringLiteral("%1:%2").arg(text(host)).arg(port), text(subject), text(issuer),
            text(fingerprint), false);
        return decision == RdpCertificateDecision::AcceptAndSave ? 1
               : decision == RdpCertificateDecision::AcceptOnce  ? 2
                                                                 : 0;
    }

    static DWORD verifyChangedCertificate(freerdp *instance, const char *host, UINT16 port,
                                          const char *, const char *subject, const char *issuer,
                                          const char *fingerprint, const char *, const char *,
                                          const char *, DWORD)
    {
        if (auto *owner = ownerOf(instance->context); !owner->m_certificateName.isEmpty())
            return owner->verifyTunnelledCertificate(text(subject), text(issuer), text(fingerprint));
        const auto decision = ownerOf(instance->context)->askCertificate(
            QStringLiteral("%1:%2").arg(text(host)).arg(port), text(subject), text(issuer),
            text(fingerprint), true);
        return decision == RdpCertificateDecision::AcceptAndSave ? 1
               : decision == RdpCertificateDecision::AcceptOnce  ? 2
                                                                 : 0;
    }

    static BOOL authenticate(freerdp *instance, char **username, char **password, char **domain,
                             rdp_auth_reason reason)
    {
        // Smartcard prompts are not supported; the server's own logon screen
        // handles the rest.
        if (reason == AUTH_SMARTCARD_PIN)
            return FALSE;
        // Log on at the remote screen: go on without credentials.
        if ((reason == AUTH_TLS || reason == AUTH_RDP)
            && !ownerOf(instance->context)->m_askCredentials)
            return TRUE;
        const bool gateway = reason == GW_AUTH_HTTP || reason == GW_AUTH_RDG
                             || reason == GW_AUTH_RPC;
        QString user = text(*username);
        QString secret = text(*password);
        QString realm = text(*domain);
        // The gateway asks before the computer does: start from the
        // session's own user when the gateway has none of its own.
        if (gateway && user.isEmpty()) {
            const auto *settings = instance->context->settings;
            user = text(freerdp_settings_get_string(settings, FreeRDP_Username));
            realm = text(freerdp_settings_get_string(settings, FreeRDP_Domain));
        }
        if (!ownerOf(instance->context)->askCredentials(&user, &secret, &realm, gateway))
            return FALSE;
        replaceString(username, user);
        replaceString(password, secret);
        replaceString(domain, realm);
        secret.fill(QChar(0));
        return TRUE;
    }
};

RdpSession::RdpSession(QObject *parent)
    : QObject(parent)
{
    m_inputEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
}

RdpSession::~RdpSession()
{
    disconnectFromHost();
    if (m_thread.joinable())
        m_thread.join();
    if (m_inputEvent)
        CloseHandle(m_inputEvent);
}

void RdpSession::connectToHost(const RdpConnectionSettings &settings)
{
    if (m_thread.joinable())
        m_thread.join();
    m_stopping = false;
    m_finished = false;
    const bool shareText = settings.options.clipboardText == 1;
    const bool shareFiles = settings.options.clipboardFiles == 1;
    m_clipboard = shareText || shareFiles
                      ? std::make_unique<RdpClipboard>(this, shareText, shareFiles)
                      : nullptr;
    m_thread = std::thread([this, settings] { run(settings); });
}

void RdpSession::disconnectFromHost()
{
    m_stopping = true;
    if (auto *context = static_cast<rdpContext *>(m_context.load()))
        freerdp_abort_connect_context(context);
}

void RdpSession::run(RdpConnectionSettings settings)
{
    m_certificateName = settings.certificateName;
    m_configPath = settings.configPath;
    QString failure;
    bool error = false;
    freerdp *instance = freerdp_new();
    if (!instance) {
        QMetaObject::invokeMethod(this, [this] {
            emit disconnected(QStringLiteral("Could not create an RDP session."), true);
        }, Qt::QueuedConnection);
        return;
    }
    instance->ContextSize = sizeof(PortableRdpContext);
    instance->PreConnect = RdpCallbacks::preConnect;
    instance->PostConnect = RdpCallbacks::postConnect;
    instance->PostDisconnect = RdpCallbacks::postDisconnect;
    instance->VerifyCertificateEx = RdpCallbacks::verifyCertificate;
    instance->VerifyChangedCertificateEx = RdpCallbacks::verifyChangedCertificate;
    instance->AuthenticateEx = RdpCallbacks::authenticate;
    instance->LoadChannels = freerdp_client_load_channels;
    // Channels are compiled into libfreerdp-client; tell the add-in loader
    // to look there (FreeRDP's own clients do this in their entry points).
    static const int registered =
        freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0);
    Q_UNUSED(registered);

    if (!freerdp_context_new(instance)) {
        freerdp_free(instance);
        QMetaObject::invokeMethod(this, [this] {
            emit disconnected(QStringLiteral("Could not create an RDP session."), true);
        }, Qt::QueuedConnection);
        return;
    }
    auto *context = instance->context;
    reinterpret_cast<PortableRdpContext *>(context)->owner = this;
    m_context = context;

    auto *rdp = context->settings;
    const auto setString = [rdp](FreeRDP_Settings_Keys_String key, const QString &value) {
        if (!value.isEmpty())
            freerdp_settings_set_string(rdp, key, value.toUtf8().constData());
    };
    setString(FreeRDP_ServerHostname, settings.host);
    setString(FreeRDP_Username, settings.username);
    setString(FreeRDP_Domain, settings.domain);
    setString(FreeRDP_Password, settings.password);
    const bool hasPassword = !settings.password.isEmpty();
    settings.password.fill(QChar(0));
    if (!settings.configPath.isEmpty()) {
        QDir().mkpath(settings.configPath);
        setString(FreeRDP_ConfigPath, QDir::toNativeSeparators(settings.configPath));
    }
    freerdp_settings_set_uint32(rdp, FreeRDP_ServerPort, settings.port);
    // Even sizes avoid codec padding artefacts on some servers.
    freerdp_settings_set_uint32(rdp, FreeRDP_DesktopWidth, qMax(640, settings.width) & ~1);
    freerdp_settings_set_uint32(rdp, FreeRDP_DesktopHeight, qMax(480, settings.height) & ~1);
    const auto &options = settings.options;
    freerdp_settings_set_uint32(rdp, FreeRDP_ColorDepth,
                                static_cast<UINT32>(options.colorDepth > 0 ? options.colorDepth : 32));
    freerdp_settings_set_bool(rdp, FreeRDP_ConsoleSession, options.adminSession == 1);
    // Visual effects follow the connection type, like mstsc's "Experience".
    freerdp_set_connection_type(rdp, options.quality == QStringLiteral("low")         ? CONNECTION_TYPE_MODEM
                                     : options.quality == QStringLiteral("broadband") ? CONNECTION_TYPE_BROADBAND_HIGH
                                                                                      : CONNECTION_TYPE_LAN);
    if (options.security != QStringLiteral("auto") && !options.security.isEmpty()) {
        // One protocol only, instead of negotiating the strongest.
        freerdp_settings_set_bool(rdp, FreeRDP_NlaSecurity, options.security == QStringLiteral("nla"));
        freerdp_settings_set_bool(rdp, FreeRDP_TlsSecurity, options.security == QStringLiteral("tls"));
        freerdp_settings_set_bool(rdp, FreeRDP_RdpSecurity, options.security == QStringLiteral("rdp"));
        freerdp_settings_set_bool(rdp, FreeRDP_ExtSecurity, FALSE);
        freerdp_settings_set_bool(rdp, FreeRDP_NegotiateSecurityLayer, FALSE);
    }
    freerdp_settings_set_bool(rdp, FreeRDP_SoftwareGdi, TRUE);
    // The RDP 8 graphics pipeline (a dynamic channel) sends only what changed,
    // with RemoteFX progressive coding, and H.264 where a decoder exists
    // (Windows Media Foundation, or FFmpeg in a developer build). It needs
    // 32-bit colour; otherwise the server sends plain bitmaps.
    const bool graphics = options.graphicsPipeline == 1
                          && freerdp_settings_get_uint32(rdp, FreeRDP_ColorDepth) == 32;
    freerdp_settings_set_bool(rdp, FreeRDP_SupportGraphicsPipeline, graphics);
    if (graphics) {
        const bool h264 = h264Available();
        freerdp_settings_set_bool(rdp, FreeRDP_GfxH264, h264);
        freerdp_settings_set_bool(rdp, FreeRDP_GfxAVC444, h264);
        freerdp_settings_set_bool(rdp, FreeRDP_GfxAVC444v2, h264);
        freerdp_settings_set_bool(rdp, FreeRDP_GfxProgressive, TRUE);
        freerdp_settings_set_bool(rdp, FreeRDP_GfxProgressiveV2, TRUE);
        freerdp_settings_set_bool(rdp, FreeRDP_RemoteFxCodec, TRUE);
        freerdp_settings_set_bool(rdp, FreeRDP_GfxThinClient, FALSE);
        freerdp_settings_set_bool(rdp, FreeRDP_GfxSmallCache, FALSE);
        QMetaObject::invokeMethod(this, [this, h264] {
            emit graphicsMode(h264 ? QStringLiteral("graphics pipeline, H.264")
                                   : QStringLiteral("graphics pipeline, RemoteFX"));
        }, Qt::QueuedConnection);
    }
    // High-DPI screens: the desktop gets the physical pixel size (set by the
    // page), and Windows scales text and icons to match.
    if (settings.scale > 1.0) {
        freerdp_settings_set_uint32(rdp, FreeRDP_DesktopScaleFactor, desktopScale(settings.scale));
        freerdp_settings_set_uint32(rdp, FreeRDP_DeviceScaleFactor, deviceScale(settings.scale));
    }
    // RD Gateway ("none" connects directly even when a default is set).
    if (!options.gateway.isEmpty() && options.gateway.compare(QStringLiteral("none"), Qt::CaseInsensitive) != 0) {
        QString gatewayHost = options.gateway;
        quint16 gatewayPort = 443;
        // "host:port"; a bare IPv6 address has several colons and no port.
        if (gatewayHost.count(QLatin1Char(':')) == 1) {
            bool ok = false;
            const uint port = gatewayHost.section(QLatin1Char(':'), 1).toUInt(&ok);
            if (ok && port > 0 && port < 65536) {
                gatewayPort = quint16(port);
                gatewayHost = gatewayHost.section(QLatin1Char(':'), 0, 0);
            }
        }
        freerdp_settings_set_bool(rdp, FreeRDP_GatewayEnabled, TRUE);
        freerdp_set_gateway_usage_method(rdp, TSC_PROXY_MODE_DIRECT);
        setString(FreeRDP_GatewayHostname, gatewayHost);
        freerdp_settings_set_uint32(rdp, FreeRDP_GatewayPort, gatewayPort);
        freerdp_settings_set_bool(rdp, FreeRDP_GatewayHttpTransport, TRUE);
        freerdp_settings_set_bool(rdp, FreeRDP_GatewayRpcTransport, TRUE);
        if (options.gatewayUser.isEmpty()) {
            freerdp_settings_set_bool(rdp, FreeRDP_GatewayUseSameCredentials, TRUE);
        } else {
            freerdp_settings_set_bool(rdp, FreeRDP_GatewayUseSameCredentials, FALSE);
            QString user = options.gatewayUser;
            QString realm;
            if (const int slash = user.indexOf(QLatin1Char('\\')); slash > 0) {
                realm = user.left(slash);
                user = user.mid(slash + 1);
            }
            setString(FreeRDP_GatewayUsername, user);
            setString(FreeRDP_GatewayDomain, realm);
        }
    }
    // Server-side support for resuming the session after a reconnect.
    freerdp_settings_set_bool(rdp, FreeRDP_AutoReconnectionEnabled, options.autoReconnect == 1);
    freerdp_settings_set_bool(rdp, FreeRDP_FastPathInput, TRUE);
    freerdp_settings_set_bool(rdp, FreeRDP_FastPathOutput, TRUE);
    freerdp_settings_set_bool(rdp, FreeRDP_IgnoreCertificate, FALSE);
    freerdp_settings_set_bool(rdp, FreeRDP_AutoAcceptCertificate, FALSE);
    freerdp_settings_set_uint32(rdp, FreeRDP_KeyboardLayout,
                                rdpKeyboardLayoutId(options.keyboardLayout));
    setString(FreeRDP_AlternateShell, options.startProgram);
    setString(FreeRDP_ShellWorkingDirectory, options.startFolder);
    // Without a password prompt, NLA (which needs credentials up front) is
    // left out so the server can show its own log-on screen.
    m_askCredentials = options.askCredentials != 0;
    if (!m_askCredentials && !hasPassword && options.security == QStringLiteral("auto"))
        freerdp_settings_set_bool(rdp, FreeRDP_NlaSecurity, FALSE);
    // Ports and WebAuthn are not redirected, nor the RDP 8 extras that need
    // device redirection.
    for (const auto key : {FreeRDP_DeviceRedirection, FreeRDP_RedirectDrives,
                           FreeRDP_RedirectHomeDrive, FreeRDP_RedirectSerialPorts,
                           FreeRDP_RedirectParallelPorts, FreeRDP_RedirectSmartCards,
                           FreeRDP_RedirectPrinters,
                           FreeRDP_AudioPlayback, FreeRDP_AudioCapture,
                           FreeRDP_RedirectWebAuthN,
                           FreeRDP_NetworkAutoDetect, FreeRDP_SupportHeartbeatPdu,
                           FreeRDP_SupportMultitransport})
        freerdp_settings_set_bool(rdp, key, FALSE);
    // Local resources. FreeRDP's channel loader (freerdp_client_load_channels)
    // adds the sound, microphone, and device channels these settings ask for.
    // A resource whose service is missing here is left out, with a note,
    // rather than failing the connection.
    QStringList skipped;
    const auto available = [&skipped](bool wanted, const QString &problem, const QString &what) {
        if (wanted && !problem.isEmpty())
            skipped.append(QStringLiteral("%1 (%2)").arg(what, problem));
        return wanted && problem.isEmpty();
    };
    const QString sound = (options.audio == QStringLiteral("local") || options.microphone == 1)
                              ? LocalResources::soundProblem() : QString();
    freerdp_settings_set_bool(rdp, FreeRDP_AudioPlayback,
                              available(options.audio == QStringLiteral("local"), sound, QStringLiteral("sound")));
    freerdp_settings_set_bool(rdp, FreeRDP_RemoteConsoleAudio, options.audio == QStringLiteral("remote"));
    freerdp_settings_set_bool(rdp, FreeRDP_AudioCapture,
                              available(options.microphone == 1, sound, QStringLiteral("microphone")));
    bool devices = false;
    const auto addDevice = [rdp, &devices](UINT32 type, const QList<QByteArray> &arguments) {
        std::vector<const char *> pointers;
        for (const auto &argument : arguments)
            pointers.push_back(argument.constData());
        RDPDR_DEVICE *device = freerdp_device_new(type, pointers.size(), pointers.data());
        if (device && freerdp_device_collection_add(rdp, device))
            devices = true;
        else if (device)
            freerdp_device_free(device);
    };
    // Drives: (name shown in the remote session, local path).
    if (options.drives == QStringLiteral("home")) {
        freerdp_settings_set_bool(rdp, FreeRDP_RedirectHomeDrive, TRUE);
        devices = true;
    } else if (options.drives == QStringLiteral("all")) {
        addDevice(RDPDR_DTYP_FILESYSTEM, {"*", "*"});
    } else if (options.drives == QStringLiteral("shared")) {
        const QString folder = portableFilePath(QStringLiteral("shared"));
        QDir().mkpath(folder);
        addDevice(RDPDR_DTYP_FILESYSTEM, {"OpsRemote", QDir::toNativeSeparators(folder).toUtf8()});
    } else if (options.drives == QStringLiteral("custom")) {
        for (const auto &path : options.driveFolders.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const QString folder = path.trimmed();
            QString name = QFileInfo(folder).fileName();
            if (name.isEmpty())
                name = folder.left(1); // a drive root, such as D:\ becomes "D"
            if (QFileInfo(folder).isDir())
                addDevice(RDPDR_DTYP_FILESYSTEM, {name.toUtf8(), QDir::toNativeSeparators(folder).toUtf8()});
        }
    }
    if (devices)
        freerdp_settings_set_bool(rdp, FreeRDP_RedirectDrives, TRUE);
    if (available(options.printers == 1, options.printers == 1 ? LocalResources::printerProblem() : QString(),
                  QStringLiteral("printers"))) {
        addDevice(RDPDR_DTYP_PRINT, {"printer"});
        freerdp_settings_set_bool(rdp, FreeRDP_RedirectPrinters, TRUE);
    }
    if (available(options.smartCards == 1, options.smartCards == 1 ? LocalResources::smartCardProblem() : QString(),
                  QStringLiteral("smart cards"))) {
        addDevice(RDPDR_DTYP_SMARTCARD, {"smartcard"});
        freerdp_settings_set_bool(rdp, FreeRDP_RedirectSmartCards, TRUE);
    }
    if (devices)
        freerdp_settings_set_bool(rdp, FreeRDP_DeviceRedirection, TRUE);
    if (!skipped.isEmpty()) {
        const QString note = QStringLiteral("Not shared with the remote computer: %1").arg(skipped.join(QStringLiteral("; ")));
        QMetaObject::invokeMethod(this, [this, note] { emit statusMessage(note); }, Qt::QueuedConnection);
    }
    // Display Control (a dynamic channel) lets the desktop follow the tab size.
    freerdp_settings_set_bool(rdp, FreeRDP_SupportDynamicChannels, TRUE);
    // The clipboard channel, when text or files are shared.
    freerdp_settings_set_bool(rdp, FreeRDP_RedirectClipboard,
                              options.clipboardText == 1 || options.clipboardFiles == 1);
    const bool follow = options.resolution == QStringLiteral("follow");
    freerdp_settings_set_bool(rdp, FreeRDP_SupportDisplayControl, follow);
    freerdp_settings_set_bool(rdp, FreeRDP_DynamicResolutionUpdate, follow);

    QMetaObject::invokeMethod(this, [this, host = settings.host, port = settings.port] {
        emit statusMessage(QStringLiteral("Connecting to %1:%2…").arg(host).arg(port));
    }, Qt::QueuedConnection);

    if (!freerdp_connect(instance)) {
        const auto code = freerdp_get_last_error(context);
        error = !m_stopping;
        failure = m_stopping ? QStringLiteral("Disconnected.")
                  : code == FREERDP_ERROR_SUCCESS
                      ? QStringLiteral("Could not connect to %1.").arg(settings.host)
                      : QStringLiteral("Could not connect: %1")
                            .arg(text(freerdp_get_last_error_string(code)));
    } else {
        const bool reconnect = settings.options.autoReconnect == 1;
        for (;;) {
            HANDLE handles[MAXIMUM_WAIT_OBJECTS];
            while (!freerdp_shall_disconnect_context(context)) {
                DWORD count = freerdp_get_event_handles(context, handles, MAXIMUM_WAIT_OBJECTS - 1);
                if (count == 0)
                    break;
                handles[count++] = m_inputEvent;
                WaitForMultipleObjects(count, handles, FALSE, 100);
                if (WaitForSingleObject(m_inputEvent, 0) == WAIT_OBJECT_0) {
                    ResetEvent(m_inputEvent);
                    flushInput(context->input);
                }
                if (!freerdp_check_event_handles(context))
                    break;
            }
            // A network drop (not the server ending the session, and not the
            // user closing the tab): try to resume the same session.
            const auto lost = freerdp_get_last_error(context);
            if (!reconnect || m_stopping || lost == FREERDP_ERROR_SUCCESS
                || lost == FREERDP_ERROR_CONNECT_CANCELLED
                || GET_FREERDP_ERROR_CLASS(lost) == FREERDP_ERROR_ERRINFO_CLASS)
                break;
            if (m_clipboard)
                m_clipboard->attach(nullptr);
            bool resumed = false;
            constexpr int attempts = 6;
            for (int attempt = 1; attempt <= attempts && !m_stopping && !resumed; ++attempt) {
                QMetaObject::invokeMethod(this, [this, attempt] {
                    emit statusMessage(QStringLiteral("Connection lost. Reconnecting (attempt %1 of %2)…")
                                           .arg(attempt)
                                           .arg(attempts));
                }, Qt::QueuedConnection);
                qInfo("RDP: reconnecting, attempt %d of %d", attempt, attempts);
                resumed = freerdp_reconnect(instance);
                for (int wait = 0; !resumed && wait < 50 && !m_stopping; ++wait)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!resumed)
                break;
            // freerdp_reconnect() keeps the GDI; report the session as back.
            if (const auto *gdi = context->gdi) {
                const int width = static_cast<int>(gdi->width);
                const int height = static_cast<int>(gdi->height);
                qInfo("RDP: reconnected");
                QMetaObject::invokeMethod(this, [this, width, height] {
                    emit connected(width, height);
                }, Qt::QueuedConnection);
            }
        }
        const auto code = freerdp_get_last_error(context);
        if (m_stopping) {
            failure = QStringLiteral("Disconnected.");
        } else if (code != FREERDP_ERROR_SUCCESS
                   && code != FREERDP_ERROR_CONNECT_CANCELLED) {
            // Server-side reasons (log off, disconnected by an administrator,
            // another user taking over) end the session normally.
            const bool serverReason = GET_FREERDP_ERROR_CLASS(code) == FREERDP_ERROR_ERRINFO_CLASS;
            error = !serverReason;
            failure = QStringLiteral("%1: %2")
                          .arg(serverReason ? QStringLiteral("Session ended")
                                            : QStringLiteral("Connection lost"),
                               text(freerdp_get_last_error_string(code)));
        } else {
            failure = QStringLiteral("The remote session ended.");
        }
        // Detach the clipboard first: once it is detached, it no longer calls
        // into the channel that freerdp_disconnect() frees.
        if (m_clipboard)
            m_clipboard->attach(nullptr);
        freerdp_disconnect(instance);
    }

    m_context = nullptr;
    if (m_clipboard)
        m_clipboard->attach(nullptr);
    m_display = nullptr;
    m_displayReady = false;
    freerdp_context_free(instance);
    freerdp_free(instance);
    QMetaObject::invokeMethod(this, [this, failure, error] {
        emit disconnected(failure, error);
    }, Qt::QueuedConnection);
    m_finished = true;
}

void RdpSession::queueInput(const InputEvent &event)
{
    {
        QMutexLocker locker(&m_inputMutex);
        m_inputQueue.append(event);
    }
    SetEvent(m_inputEvent);
}

void RdpSession::flushInput(void *inputPointer)
{
    auto *input = static_cast<rdpInput *>(inputPointer);
    QList<InputEvent> events;
    {
        QMutexLocker locker(&m_inputMutex);
        events.swap(m_inputQueue);
    }
    for (const auto &event : std::as_const(events)) {
        switch (event.type) {
        case InputEvent::Key:
            freerdp_input_send_keyboard_event_ex(input, event.down, event.repeat, event.code);
            break;
        case InputEvent::Unicode:
            freerdp_input_send_unicode_keyboard_event(input, event.down ? 0 : KBD_FLAGS_RELEASE,
                                                      static_cast<UINT16>(event.code));
            break;
        case InputEvent::Pause:
            freerdp_input_send_keyboard_pause_event(input);
            break;
        case InputEvent::Mouse:
            freerdp_input_send_mouse_event(input, event.flags, static_cast<UINT16>(event.x),
                                           static_cast<UINT16>(event.y));
            break;
        case InputEvent::ExtendedMouse:
            freerdp_input_send_extended_mouse_event(input, event.flags,
                                                    static_cast<UINT16>(event.x),
                                                    static_cast<UINT16>(event.y));
            break;
        case InputEvent::Synchronize:
            freerdp_input_send_synchronize_event(input, event.code);
            break;
        case InputEvent::Resize:
            if (m_display && m_displayReady) {
                QList<QRect> monitors;
                qreal scale = 1.0;
                {
                    QMutexLocker locker(&m_inputMutex);
                    monitors = m_monitorLayout;
                    scale = m_layoutScale;
                }
                if (monitors.size() > int(m_maxMonitors))
                    monitors = {monitors.first()};
                QList<DISPLAY_CONTROL_MONITOR_LAYOUT> layouts;
                for (qsizetype i = 0; i < monitors.size(); ++i) {
                    const QRect &monitor = monitors[i];
                    DISPLAY_CONTROL_MONITOR_LAYOUT layout{};
                    // The first one is the primary monitor, at 0,0.
                    layout.Flags = i == 0 ? DISPLAY_CONTROL_MONITOR_PRIMARY : 0;
                    layout.Left = monitor.left();
                    layout.Top = monitor.top();
                    // Even sizes of 200 to 8192 pixels (the protocol wants an even width;
                    // Windows ignores an odd height with the graphics pipeline).
                    layout.Width = static_cast<UINT32>(qBound(200, monitor.width(), 8192)) & ~1u;
                    layout.Height = static_cast<UINT32>(qBound(200, monitor.height(), 8192)) & ~1u;
                    layout.Orientation = ORIENTATION_LANDSCAPE;
                    layout.DesktopScaleFactor = desktopScale(scale);
                    layout.DeviceScaleFactor = deviceScale(scale);
                    layout.PhysicalWidth = 0;
                    layout.PhysicalHeight = 0;
                    layouts.append(layout);
                }
                if (!layouts.isEmpty()) {
                    auto *display = static_cast<DispClientContext *>(m_display);
                    (void)display->SendMonitorLayout(display, UINT32(layouts.size()), layouts.data());
                }
            }
            break;
        }
    }
}

void RdpSession::sendKey(quint32 rdpScancode, bool down, bool repeat)
{
    queueInput({InputEvent::Key, rdpScancode, down, repeat});
}

void RdpSession::sendUnicode(quint16 character, bool down)
{
    queueInput({InputEvent::Unicode, character, down});
}

void RdpSession::sendPause()
{
    queueInput({InputEvent::Pause});
}

void RdpSession::sendMouse(quint16 flags, int x, int y)
{
    queueInput({InputEvent::Mouse, 0, false, false, flags, x, y});
}

void RdpSession::sendExtendedMouse(quint16 flags, int x, int y)
{
    queueInput({InputEvent::ExtendedMouse, 0, false, false, flags, x, y});
}

void RdpSession::setLocalClipboard(const QString &text, const QStringList &files)
{
    if (m_clipboard)
        m_clipboard->setLocal(text, files);
}

void RdpSession::saveRemoteFiles(const QString &folder)
{
    if (m_clipboard)
        m_clipboard->saveRemoteFiles(folder);
}

void RdpSession::cancelRemoteFileSave()
{
    if (m_clipboard)
        m_clipboard->cancelSave();
}

void RdpSession::requestDesktopSize(int width, int height, qreal scale)
{
    requestMonitorLayout({QRect(0, 0, width, height)}, scale);
}

void RdpSession::requestMonitorLayout(const QList<QRect> &monitors, qreal scale)
{
    if (monitors.isEmpty())
        return;
    {
        QMutexLocker locker(&m_inputMutex);
        m_monitorLayout = monitors;
        m_layoutScale = scale;
    }
    queueInput({InputEvent::Resize});
}

int RdpSession::maxMonitors() const
{
    return m_displayReady ? int(m_maxMonitors) : 1;
}

void RdpSession::sendSynchronize(quint32 toggleStates)
{
    queueInput({InputEvent::Synchronize, toggleStates});
}

void RdpSession::resizeFrame(int width, int height)
{
    QMutexLocker locker(&m_frameMutex);
    if (m_frame.width() != width || m_frame.height() != height) {
        m_frame = QImage(width, height, QImage::Format_RGB32);
        m_frame.fill(Qt::black);
    }
}

void RdpSession::copyFrame(const unsigned char *buffer, int stride, int width, int height,
                           const QRect &area)
{
    const QRect clipped = area & QRect(0, 0, width, height);
    if (clipped.isEmpty() || !buffer)
        return;
    {
        QMutexLocker locker(&m_frameMutex);
        if (m_frame.width() != width || m_frame.height() != height)
            return;
        // BGRX32 in memory is QImage::Format_RGB32 on little-endian machines.
        for (int row = clipped.top(); row <= clipped.bottom(); ++row) {
            std::memcpy(m_frame.scanLine(row) + clipped.left() * 4,
                        buffer + row * stride + clipped.left() * 4,
                        static_cast<size_t>(clipped.width()) * 4);
        }
    }
    postFrameUpdate(clipped);
}

// Coalesces repaint requests: one queued update per GUI event-loop pass.
void RdpSession::postFrameUpdate(const QRect &area)
{
    QMutexLocker locker(&m_frameMutex);
    m_pendingUpdate |= area;
    if (m_updatePosted)
        return;
    m_updatePosted = true;
    QMetaObject::invokeMethod(this, [this] {
        QRect update;
        {
            QMutexLocker locker(&m_frameMutex);
            update = m_pendingUpdate;
            m_pendingUpdate = QRect();
            m_updatePosted = false;
        }
        emit frameUpdated(update);
    }, Qt::QueuedConnection);
}

// Runs ask on the GUI thread and waits for it, but stops waiting (and gives
// up) once the session is disconnecting, so closing a tab or the window never
// waits for a dialog the worker asked for. The state is shared: if the
// session goes away first, the queued call is dropped with it.
template <typename Ask>
bool RdpSession::askOnGuiThread(Ask ask)
{
    struct Prompt
    {
        std::mutex mutex;
        std::condition_variable answered;
        bool done = false;
    };
    auto prompt = std::make_shared<Prompt>();
    QMetaObject::invokeMethod(this, [prompt, ask] {
        ask();
        std::lock_guard<std::mutex> lock(prompt->mutex);
        prompt->done = true;
        prompt->answered.notify_all();
    }, Qt::QueuedConnection);
    std::unique_lock<std::mutex> lock(prompt->mutex);
    while (!prompt->done) {
        if (m_stopping)
            return false;
        prompt->answered.wait_for(lock, std::chrono::milliseconds(100));
    }
    return true;
}

unsigned long RdpSession::verifyTunnelledCertificate(const QString &subject,
                                                     const QString &issuer,
                                                     const QString &fingerprint)
{
    // One "host:port fingerprint" line per server.
    const QString path = QDir(m_configPath).filePath(QStringLiteral("tunnelled_certificates"));
    QStringList lines;
    QString saved;
    if (QFile file(path); file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const auto &line : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'),
                                                                        Qt::SkipEmptyParts)) {
            if (line.section(QLatin1Char(' '), 0, 0) == m_certificateName)
                saved = line.section(QLatin1Char(' '), 1);
            else
                lines.append(line);
        }
    }
    if (!saved.isEmpty() && saved == fingerprint)
        return 2;
    const auto decision = askCertificate(m_certificateName, subject, issuer, fingerprint,
                                         !saved.isEmpty());
    if (decision == RdpCertificateDecision::Reject)
        return 0;
    if (decision == RdpCertificateDecision::AcceptAndSave) {
        lines.append(m_certificateName + QLatin1Char(' ') + fingerprint);
        QDir().mkpath(m_configPath);
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write((lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
            if (file.commit())
                restrictToOwner(path);
        }
    }
    // Never saved in FreeRDP's own store, where the name is only 127.0.0.1.
    return 2;
}

RdpCertificateDecision RdpSession::askCertificate(const QString &host, const QString &subject,
                                                  const QString &issuer,
                                                  const QString &fingerprint, bool changed)
{
    if (m_stopping)
        return RdpCertificateDecision::Reject;
    // Answers are filled in on the GUI thread; shared so they outlive an
    // abandoned wait.
    auto decision = std::make_shared<RdpCertificateDecision>(RdpCertificateDecision::Reject);
    if (!askOnGuiThread([this, host, subject, issuer, fingerprint, changed, decision] {
            emit certificateVerificationRequired(host, subject, issuer, fingerprint, changed,
                                                 decision.get());
        }))
        return RdpCertificateDecision::Reject;
    return *decision;
}

bool RdpSession::askCredentials(QString *username, QString *password, QString *domain,
                                bool gateway)
{
    if (m_stopping)
        return false;
    struct Answer
    {
        QString username, password, domain;
        bool accepted = false;
    };
    auto answer = std::make_shared<Answer>(Answer{*username, QString(), *domain});
    if (!askOnGuiThread([this, answer, gateway] {
            emit credentialsRequired(&answer->username, &answer->password, &answer->domain,
                                     gateway, &answer->accepted);
        }))
        return false;
    if (!answer->accepted)
        return false;
    *username = answer->username;
    *password = answer->password;
    *domain = answer->domain;
    answer->password.fill(QChar(0));
    return true;
}

void RdpSession::retire()
{
    disconnectFromHost();
    // Nobody listens any more; the session deletes itself once its thread
    // has ended (FreeRDP may take a moment to return from a connection
    // attempt), instead of making the GUI wait for it.
    disconnect(this, nullptr, nullptr, nullptr);
    setParent(nullptr);
    auto *poll = new QTimer(this);
    connect(poll, &QTimer::timeout, this, [this] {
        if (!m_finished)
            return;
        if (m_thread.joinable())
            m_thread.join();
        deleteLater();
    });
    poll->start(100);
}
