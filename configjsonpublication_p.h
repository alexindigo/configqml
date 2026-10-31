// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configjsonbackend.h"
#include <sys/stat.h>
#include <unistd.h>

namespace configqml {
struct ConfigFileDescriptor {
    int fd = -1;
    explicit ConfigFileDescriptor(int value = -1) : fd(value) {}
    ConfigFileDescriptor(const ConfigFileDescriptor &) = delete;
    ConfigFileDescriptor &operator=(const ConfigFileDescriptor &) = delete;
    ~ConfigFileDescriptor() {
        if (fd >= 0)
            ::close(fd);
    }
};
QByteArray fileIdentityStamp(const struct stat &state);
QByteArray fileIdentityStampAt(int directory, const QString &name);
ConfigError checkFileIdentityAt(int directory, const QString &name, const QString &id, bool writing,
                                const QString &diagnosticFile);
int openPublicationLock(int directory, const QString &name);

struct ConfigJsonBackend::PublicationOptions {
    enum class Mode { Replace, KeepExisting, Remove };
    Mode mode = Mode::Replace;
    bool preservePermissions = true;
    std::function<ConfigError()> validate;
    std::function<void(const QByteArray &)> published;
    // Callers own protocol-specific phase/error vocabulary, not staging mechanics.
    std::function<QString(const QString &)> phase;
    std::function<ConfigError(ConfigError)> translateError;
};
struct ConfigJsonBackend::PublicationResult {
    ConfigError error;
    bool published = false;
    bool existing = false;
};
} // namespace configqml
