// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "reconnect.h"

#include <QSpinBox>

#include <yaml-cpp/yaml.h>

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

void writeReconnectTiming(YAML::Node &node, int delay, int attempts)
{
    if (delay > 0)
        node["reconnect_delay"] = delay;
    if (attempts >= 0)
        node["reconnect_attempts"] = attempts;
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
    if (inheritedDelay >= 0)
        box->setSpecialValueText(QStringLiteral("%1 s").arg(inheritedDelay));
        box->setProperty("inherits", true); // applyInheritance()
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
