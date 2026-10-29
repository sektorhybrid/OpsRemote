// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "castrecorder.h"
#include "terminalappearance.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include <qtermwidget.h>

namespace {

// Pauses longer than this play as this long.
constexpr double MaximumIdle = 2.0;
// A recording to play is read whole; these bound what that may cost.
constexpr qint64 MaximumRecordingBytes = 512LL * 1024 * 1024;
constexpr qsizetype MaximumEvents = 2000000;

QString clockText(double seconds)
{
    const int whole = int(seconds);
    return QStringLiteral("%1:%2").arg(whole / 60).arg(whole % 60, 2, 10, QLatin1Char('0'));
}

}

// ---- Recording ----

CastRecorder::~CastRecorder()
{
    stop();
}

bool CastRecorder::start(const QString &path, int columns, int rows, const QString &title,
                         QString *error)
{
    stop();
    QDir().mkpath(QFileInfo(path).absolutePath());
    auto *file = new QFile(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = file->errorString();
        delete file;
        return false;
    }
    m_file = file;
    m_path = path;
    m_failure.clear();
    m_decoder.resetState();
    QJsonObject header{{QStringLiteral("version"), 2},
                       {QStringLiteral("width"), columns},
                       {QStringLiteral("height"), rows},
                       {QStringLiteral("timestamp"), QDateTime::currentSecsSinceEpoch()},
                       {QStringLiteral("title"), title},
                       {QStringLiteral("env"),
                        QJsonObject{{QStringLiteral("TERM"), QStringLiteral("xterm-256color")}}}};
    if (!write(QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n')) {
        if (error)
            *error = m_failure;
        return false;
    }
    m_clock.start();
    return true;
}

void CastRecorder::output(const QByteArray &data)
{
    if (m_file && !data.isEmpty())
        writeEvent("o", m_decoder.decode(data));
}

void CastRecorder::resize(int columns, int rows)
{
    if (m_file)
        writeEvent("r", QStringLiteral("%1x%2").arg(columns).arg(rows));
}

void CastRecorder::stop()
{
    if (!m_file)
        return;
    m_file->close();
    delete m_file;
    m_file = nullptr;
}

void CastRecorder::writeEvent(const char *type, const QString &text)
{
    if (text.isEmpty())
        return;
    const QJsonArray event{double(m_clock.nsecsElapsed()) / 1e9, QLatin1String(type), text};
    write(QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n');
}

// A line that cannot be written ends the recording, keeping what was saved.
bool CastRecorder::write(const QByteArray &line)
{
    if (m_file->write(line) == line.size() && m_file->flush())
        return true;
    m_failure = m_file->errorString();
    if (m_failure.isEmpty())
        m_failure = QStringLiteral("the file could not be written");
    stop();
    return false;
}

// ---- Playback ----

CastPlayer::CastPlayer(QWidget *parent) : QDialog(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 560);
    m_terminal = new QTermWidget(0, this);
    registerTerminalResources(m_terminal);
    applyTerminalAppearance(m_terminal, TerminalAppearance());
    m_terminal->startTerminalTeletype();
    // Typing does nothing: this is a recording.
    m_terminal->setFlowControlEnabled(false);

    m_playButton = new QPushButton(QStringLiteral("Pause"), this);
    m_speed = new QComboBox(this);
    for (const auto &[label, factor] : QList<QPair<QString, double>>{
             {QStringLiteral("0.5×"), 0.5}, {QStringLiteral("1×"), 1.0},
             {QStringLiteral("2×"), 2.0}, {QStringLiteral("4×"), 4.0}, {QStringLiteral("8×"), 8.0}})
        m_speed->addItem(label, factor);
    m_speed->setCurrentIndex(1);
    m_slider = new QSlider(Qt::Horizontal, this);
    m_timeLabel = new QLabel(this);

    auto *controls = new QHBoxLayout;
    controls->addWidget(m_playButton);
    controls->addWidget(m_slider, 1);
    controls->addWidget(m_timeLabel);
    controls->addWidget(new QLabel(QStringLiteral("Speed:"), this));
    controls->addWidget(m_speed);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_terminal, 1);
    layout->addLayout(controls);

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &CastPlayer::scheduleNext);
    // Position updates while playing.
    auto *ticker = new QTimer(this);
    ticker->start(250);
    connect(ticker, &QTimer::timeout, this, &CastPlayer::updatePosition);

    connect(m_playButton, &QPushButton::clicked, this, [this] {
        if (m_playing) {
            m_position += m_clock.elapsed() / 1000.0 * m_speed->currentData().toDouble();
            m_playing = false;
            m_timer->stop();
        } else {
            if (m_next >= m_events.size())
                playFrom(0);
            m_playing = true;
            m_clock.start();
            scheduleNext();
        }
        m_playButton->setText(m_playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
    });
    connect(m_speed, &QComboBox::currentIndexChanged, this, [this] {
        // Keep the place, then go on at the new speed.
        if (m_playing) {
            m_position = m_events.value(qMax(0, m_next - 1)).time;
            m_clock.start();
            scheduleNext();
        }
    });
    connect(m_slider, &QSlider::sliderReleased, this, [this] {
        const double target = m_slider->value() / 10.0;
        // The events are in time order.
        const auto found = std::upper_bound(m_events.cbegin(), m_events.cend(), target,
                                            [](double time, const Event &event) { return time < event.time; });
        seekTo(int(found - m_events.cbegin()));
        m_position = target;
        m_clock.start();
        scheduleNext();
    });
}

bool CastPlayer::load(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (file.size() > MaximumRecordingBytes) {
        if (error)
            *error = QStringLiteral("The recording is larger than %1 MB, more than OpsRemote plays.")
                         .arg(MaximumRecordingBytes / (1024 * 1024));
        return false;
    }
    const auto header = QJsonDocument::fromJson(file.readLine()).object();
    if (header.value(QStringLiteral("version")).toInt() != 2) {
        if (error)
            *error = QStringLiteral("This is not an asciinema version 2 recording.");
        return false;
    }
    setWindowTitle(QStringLiteral("Recording: %1").arg(QFileInfo(path).fileName()));
    m_events.clear();
    double last = 0;
    double shifted = 0; // idle time taken out so far
    while (!file.atEnd()) {
        const auto event = QJsonDocument::fromJson(file.readLine()).array();
        if (event.size() < 3)
            continue;
        const QString type = event[1].toString();
        if (type != QStringLiteral("o") && type != QStringLiteral("r"))
            continue;
        // Times must be numbers that do not go back (a damaged or edited
        // file is played in its order).
        double time = event[0].toDouble(-1);
        if (!std::isfinite(time) || time < 0)
            continue;
        time = qMax(time, last);
        if (m_events.size() >= MaximumEvents) {
            if (error)
                *error = QStringLiteral("The recording has more than %1 events, more than OpsRemote plays.")
                             .arg(MaximumEvents);
            m_events.clear();
            return false;
        }
        if (time - last > MaximumIdle)
            shifted += time - last - MaximumIdle;
        last = time;
        m_events.append({time - shifted, type == QStringLiteral("r"), event[2].toString()});
    }
    m_duration = m_events.isEmpty() ? 0 : m_events.constLast().time;
    m_slider->setRange(0, int(m_duration * 10));
    playFrom(0);
    m_playing = true;
    m_clock.start();
    scheduleNext();
    return true;
}

void CastPlayer::seekTo(int index)
{
    if (index < m_next) {
        playFrom(index);
        return;
    }
    m_timer->stop();
    while (m_next < index && m_next < m_events.size())
        apply(m_events[m_next++]);
    m_position = m_next > 0 ? m_events[m_next - 1].time : 0;
    updatePosition();
}

// Shows the recording up to (not including) event index, at once.
void CastPlayer::playFrom(int index)
{
    m_timer->stop();
    const QByteArray reset("\x1b" "c"); // full terminal reset
    m_terminal->receiveData(reset.constData(), int(reset.size()));
    m_terminal->clear();
    for (int i = 0; i < index && i < m_events.size(); ++i)
        apply(m_events[i]);
    m_next = index;
    m_position = index > 0 ? m_events[index - 1].time : 0;
    updatePosition();
}

void CastPlayer::scheduleNext()
{
    if (!m_playing)
        return;
    const double speed = m_speed->currentData().toDouble();
    const double now = m_position + m_clock.elapsed() / 1000.0 * speed;
    while (m_next < m_events.size() && m_events[m_next].time <= now)
        apply(m_events[m_next++]);
    if (m_next >= m_events.size()) {
        m_playing = false;
        m_position = m_duration;
        m_playButton->setText(QStringLiteral("Play"));
        updatePosition();
        return;
    }
    m_timer->start(qMax(0, int((m_events[m_next].time - now) / speed * 1000)));
}

void CastPlayer::apply(const Event &event)
{
    if (event.resize)
        return; // the window keeps its size; full-screen programs redraw
    const QByteArray bytes = event.text.toUtf8();
    m_terminal->receiveData(bytes.constData(), int(bytes.size()));
}

void CastPlayer::updatePosition()
{
    double now = m_position;
    if (m_playing)
        now = qMin(m_duration, m_position + m_clock.elapsed() / 1000.0 * m_speed->currentData().toDouble());
    if (!m_slider->isSliderDown())
        m_slider->setValue(int(now * 10));
    m_timeLabel->setText(QStringLiteral("%1 / %2").arg(clockText(now), clockText(m_duration)));
}
