// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QProxyStyle>

// The "OpsRemote" style: Fusion with a calmer, more current look. Buttons,
// fields and combo boxes have rounded corners, the focused field shows the
// accent colour, scroll bars are slim pills without arrow buttons, and tabs
// are flat with an accent line under the current one. Colours come from the
// palette, so every colour theme applies.
class OpsRemoteStyle final : public QProxyStyle
{
    Q_OBJECT

public:
    OpsRemoteStyle();

    static QString key() { return QStringLiteral("OpsRemote"); }

    void polish(QWidget *widget) override;
    void unpolish(QWidget *widget) override;
    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr,
                    const QWidget *widget = nullptr) const override;
    int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
                  const QWidget *widget = nullptr,
                  QStyleHintReturn *returnData = nullptr) const override;
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                     const QWidget *widget = nullptr) const override;
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option,
                            QPainter *painter, const QWidget *widget = nullptr) const override;
    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option,
                         SubControl subControl, const QWidget *widget = nullptr) const override;
};
