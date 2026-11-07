// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QLockFile>
#include <QStringList>

namespace YAML { class Node; }

// settings/settings.yaml carries "settings_version". When a setting is
// renamed, moved, split, or its values change, a step that rewrites the old
// form into the new one is added in settingsmigrations.cpp and this number
// goes up. Files without the key are version 0 (0.6.1 and earlier).
constexpr int CurrentSettingsVersion = 5;

// Held while settings.yaml is read, changed and written back, so that two
// OpsRemote programs on the same settings (Single instance off, or the Linux
// and the Windows program on one shared folder) do not undo each other's
// change. A lock left by a program that crashed is taken over after 10 s
// (from the other system it cannot be told whether that program still
// runs). If the lock is not free within 3 s, the change is saved anyway: a
// lost race is better than a lost change.
class SettingsFileLock
{
public:
    explicit SettingsFileLock(const QString &settingsFile)
        : m_lock(settingsFile + QStringLiteral(".lock"))
    {
        m_lock.setStaleLockTime(10000);
        if (!m_lock.tryLock(3000))
            qWarning("settings: %s is locked by another OpsRemote; saving anyway",
                     qUtf8Printable(settingsFile));
    }

private:
    QLockFile m_lock; // unlocked when destroyed
};

// Brings settings.yaml up to CurrentSettingsVersion at start, before anything
// reads it, after backing up the settings folder. A file from a newer
// OpsRemote is left as it is. Returns what was done, for the log.
QStringList migrateSettings();

// The steps on a loaded document (for tests): returns the version it had,
// and fills done with one line per step applied.
int migrateSettingsDocument(YAML::Node &settings, QStringList *done = nullptr);
