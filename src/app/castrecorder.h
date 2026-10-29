// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QList>
#include <QString>
#include <QStringDecoder>

class QFile;
class QLabel;
class QComboBox;
class QPushButton;
class QSlider;
class QTermWidget;
class QTimer;

// Writes a terminal's output as an asciinema v2 recording (.cast): a JSON
// header line, then one [seconds, "o", text] line per output and
// [seconds, "r", "COLSxROWS"] per resize. Plays in asciinema and in
// OpsRemote (CastPlayer).
class CastRecorder final
{
public:
    ~CastRecorder();

    bool start(const QString &path, int columns, int rows, const QString &title,
               QString *error = nullptr);
    void output(const QByteArray &data);
    void resize(int columns, int rows);
    void stop();
    bool isRecording() const { return m_file != nullptr; }
    QString path() const { return m_path; }
    // Why the recording stopped by itself (the disk was full, the folder
    // went away); empty while it runs.
    QString failure() const { return m_failure; }

private:
    void writeEvent(const char *type, const QString &text);
    bool write(const QByteArray &line);

    QFile *m_file = nullptr;
    QString m_path;
    QString m_failure;
    QElapsedTimer m_clock;
    // Output may split a UTF-8 character between two reads.
    QStringDecoder m_decoder{QStringDecoder::Utf8};
};

// Plays a .cast recording in a terminal, with pause, speed, and a position
// slider. Long pauses in the recording are shortened to two seconds.
class CastPlayer final : public QDialog
{
    Q_OBJECT

public:
    explicit CastPlayer(QWidget *parent = nullptr);

    bool load(const QString &path, QString *error = nullptr);

private:
    struct Event
    {
        double time = 0; // seconds, idle time already shortened
        bool resize = false;
        QString text; // output, or "COLSxROWS"
    };

    void playFrom(int index);
    // Moves to event index: forward by playing the events between, back by
    // starting over.
    void seekTo(int index);
    void scheduleNext();
    void apply(const Event &event);
    void updatePosition();

    QList<Event> m_events;
    int m_next = 0;
    double m_duration = 0;
    double m_position = 0; // seconds of the recording shown
    bool m_playing = false;
    QElapsedTimer m_clock; // since m_position was set
    QTermWidget *m_terminal = nullptr;
    QTimer *m_timer = nullptr;
    QPushButton *m_playButton = nullptr;
    QComboBox *m_speed = nullptr;
    QSlider *m_slider = nullptr;
    QLabel *m_timeLabel = nullptr;
};
