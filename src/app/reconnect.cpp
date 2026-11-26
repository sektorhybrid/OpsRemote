// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "reconnect.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QTimer>

#include <yaml-cpp/yaml.h>

namespace {

constexpr int DefaultReconnectDelay = 5; // seconds
constexpr int DefaultReconnectAttempts = 5; // 0: keep trying

// reconnect_delay (1–600 s) and reconnect_attempts (0–1000); missing or
// invalid keys leave the values as they are.
void readReconnectTiming(const YAML::Node &node, int &delay, int &attempts)
{
    try {
        if (node["reconnect_delay"])
            delay = qBound(1, node["reconnect_delay"].as<int>(), 600);
    } catch (const YAML::Exception &) {
    }
    try {
        if (node["reconnect_attempts"])
            attempts = qBound(0, node["reconnect_attempts"].as<int>(), 1000);
    } catch (const YAML::Exception &) {
    }
}

// Only set values: a session's 0 delay and -1 tries mean "the Preferences'".
void writeReconnectTiming(YAML::Node &node, int delay, int attempts)
{
    if (delay > 0)
        node["reconnect_delay"] = delay;
    if (attempts >= 0)
        node["reconnect_attempts"] = attempts;
}

}

QString reconnectAttemptText(int attempt, int attempts)
{
    return attempts == 0 ? QStringLiteral("attempt %1").arg(attempt)
                         : QStringLiteral("attempt %1 of %2").arg(attempt).arg(attempts);
}

QSpinBox *reconnectDelayBox(int value, int inheritedDelay)
{
    auto *box = new QSpinBox;
    box->setRange(inheritedDelay >= 0 ? 0 : 1, 600);
    box->setSuffix(QStringLiteral(" s"));
    if (inheritedDelay >= 0) {
        box->setSpecialValueText(QStringLiteral("%1 s").arg(inheritedDelay));
        box->setProperty("inherits", true); // applyInheritance()
    }
    box->setValue(value);
    box->setToolTip(QStringLiteral("How long to wait before each try"));
    return box;
}

QSpinBox *reconnectAttemptsBox(int value, int inheritedAttempts)
{
    // 0 keeps trying; shown as words, as the lowest value is the inherited one
    // in a session's window.
    class Box final : public QSpinBox
    {
    public:
        using QSpinBox::QSpinBox;

    protected:
        QString textFromValue(int v) const override
        {
            return v == 0 ? QStringLiteral("Keep trying") : QSpinBox::textFromValue(v);
        }
        int valueFromText(const QString &text) const override
        {
            return text.startsWith(QStringLiteral("Keep"), Qt::CaseInsensitive) ? 0 : QSpinBox::valueFromText(text);
        }
        QValidator::State validate(QString &text, int &position) const override
        {
            if (QStringLiteral("Keep trying").startsWith(text, Qt::CaseInsensitive))
                return text.size() == 11 ? QValidator::Acceptable : QValidator::Intermediate;
            return QSpinBox::validate(text, position);
        }
    };
    auto *box = new Box;
    const bool session = inheritedAttempts >= -1;
    box->setRange(session ? -1 : 0, 1000);
    if (session) {
        box->setSpecialValueText(inheritedAttempts == 0 ? QStringLiteral("Keep trying")
                                                        : QString::number(inheritedAttempts));
        box->setProperty("inherits", true); // applyInheritance()
    }
    box->setValue(value);
    box->setToolTip(QStringLiteral("How many tries before giving up; 0: keep trying"));
    return box;
}

namespace {

// Wait and Tries rows under the switch, usable only while it is on.
void addReconnectRows(QFormLayout *form, QCheckBox *autoReconnect, QSpinBox *delay, QSpinBox *attempts)
{
    form->addRow(QStringLiteral("Wait:"), delay);
    form->addRow(QStringLiteral("Tries:"), attempts);
    for (QWidget *box : {static_cast<QWidget *>(delay), static_cast<QWidget *>(attempts)}) {
        box->setEnabled(autoReconnect->isChecked());
        QObject::connect(autoReconnect, &QCheckBox::toggled, box, &QWidget::setEnabled);
    }
}

}

ReconnectOptions ReconnectOptions::overriddenBy(const ReconnectOptions &session) const
{
    ReconnectOptions result = *this;
    if (session.autoReconnect >= 0)
        result.autoReconnect = session.autoReconnect;
    if (session.delay > 0)
        result.delay = session.delay;
    if (session.attempts >= 0)
        result.attempts = session.attempts;
    return result;
}

ReconnectOptions ReconnectOptions::builtIn()
{
    ReconnectOptions options;
    options.autoReconnect = 0;
    options.delay = DefaultReconnectDelay;
    options.attempts = DefaultReconnectAttempts;
    return options;
}

ReconnectOptions reconnectOptionsFromYaml(const YAML::Node &node)
{
    ReconnectOptions options;
    if (!node.IsMap())
        return options;
    try {
        if (node["auto_reconnect"])
            options.autoReconnect = node["auto_reconnect"].as<bool>() ? 1 : 0;
    } catch (const YAML::Exception &) {
    }
    readReconnectTiming(node, options.delay, options.attempts);
    return options;
}

void reconnectOptionsToYaml(const ReconnectOptions &options, YAML::Node &node)
{
    if (options.autoReconnect >= 0)
        node["auto_reconnect"] = options.autoReconnect == 1;
    writeReconnectTiming(node, options.delay, options.attempts);
}

ReconnectEditorRows::ReconnectEditorRows(QFormLayout *form, const QString &switchText,
                                         const ReconnectOptions &value, const ReconnectOptions &defaults,
                                         QWidget *parent, const QString &toolTip)
    : m_inheritedOn(defaults.autoReconnect == 1 ? 1 : 0)
{
    m_switch = new QCheckBox(switchText, parent);
    m_switch->setToolTip(toolTip);
    m_switch->setChecked((value.autoReconnect >= 0 ? value.autoReconnect : m_inheritedOn) == 1);
    m_switch->setProperty("inheritedOn", m_inheritedOn == 1); // applyInheritance()
    m_delay = reconnectDelayBox(value.delay, defaults.delay);
    m_attempts = reconnectAttemptsBox(value.attempts, defaults.attempts);
    form->addRow(QString(), m_switch);
    addReconnectRows(form, m_switch, m_delay, m_attempts);
}

ReconnectOptions ReconnectEditorRows::value() const
{
    ReconnectOptions options;
    const int on = m_switch->isChecked() ? 1 : 0;
    options.autoReconnect = on == m_inheritedOn ? -1 : on;
    options.delay = m_delay->value();
    options.attempts = m_attempts->value();
    return options;
}

QString Reconnector::retry(const ReconnectOptions &options, QObject *context, const QString &what,
                           const std::function<void()> &start)
{
    if (!mayReconnect(m_made, options.attempts))
        return {};
    ++m_made;
    const QString attempt = reconnectAttemptText(m_made, options.attempts);
    qInfo("Connection: %s: reconnecting in %d s (%s)", qUtf8Printable(what), options.delay, qUtf8Printable(attempt));
    QTimer::singleShot(options.delay * 1000, context, start);
    return attempt;
}
