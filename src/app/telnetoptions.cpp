// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "telnetoptions.h"
#include "formcard.h"
#include "preferences.h"

#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <yaml-cpp/yaml.h>

TelnetOptions TelnetOptions::overriddenBy(const TelnetOptions &session) const
{
    TelnetOptions result = *this;
    result.reconnect = reconnect.overriddenBy(session.reconnect);
    return result;
}

TelnetOptions TelnetOptions::builtIn()
{
    TelnetOptions options;
    options.reconnect = ReconnectOptions::builtIn();
    return options;
}

TelnetOptions telnetOptionsFromYaml(const YAML::Node &node)
{
    TelnetOptions options;
    if (node && node.IsMap())
        options.reconnect = reconnectOptionsFromYaml(node);
    return options;
}

void telnetOptionsToYaml(const TelnetOptions &options, YAML::Node &node)
{
    reconnectOptionsToYaml(options.reconnect, node);
}

TelnetOptions loadTelnetDefaults()
{
    try {
        return TelnetOptions::builtIn().overriddenBy(telnetOptionsFromYaml(loadPreferencesSection("telnet")));
    } catch (const YAML::Exception &) {
        return TelnetOptions::builtIn();
    }
}

void saveTelnetDefaults(const TelnetOptions &options)
{
    YAML::Node node(YAML::NodeType::Map);
    telnetOptionsToYaml(options, node);
    savePreferencesSection("telnet", node);
}

TelnetOptionsEditor::TelnetOptionsEditor(const TelnetOptions &value, const TelnetOptions &defaults,
                                         QWidget *parent)
    : QWidget(parent)
{
    const TelnetOptions fallback = TelnetOptions::builtIn().overriddenBy(defaults);
    // A page of the session window's terminal tabs ("Connection"), in light
    // cards (formcard.h).
    auto *cards = new QVBoxLayout(this);
    cards->setContentsMargins(0, 0, 0, 0);
    auto *form = addFormCard(cards, QStringLiteral("Reconnect"), this);
    m_reconnect = std::make_unique<ReconnectEditorRows>(
        form, QStringLiteral("Reconnect automatically if the network drops"), value.reconnect,
        fallback.reconnect, this);
    auto *note = new QLabel(QStringLiteral(
        "Telnet cannot tell a logout from a cut connection: it reconnects after a network error, "
        "or when the server closes after 20 s without output (a logout prints something first). "
        "A dead network is noticed within about a minute. Telnet is not encrypted."), this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(note);
    cards->addStretch();
    alignFormLabels(this);
}

TelnetOptionsEditor::~TelnetOptionsEditor() = default;

TelnetOptions TelnetOptionsEditor::value() const
{
    TelnetOptions options;
    options.reconnect = m_reconnect->value();
    return options;
}
