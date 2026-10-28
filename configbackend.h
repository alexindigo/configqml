// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <optional>

namespace configqml {
struct ConfigError {
    ConfigError() = default;
    ConfigError(QString code, QString sourceId, QString operation, QString file = {},
                QStringList path = {}, qsizetype line = 0)
        : code(std::move(code)), sourceId(std::move(sourceId)), operation(std::move(operation)),
          file(std::move(file)), path(std::move(path)), line(line) {}
    QString code;
    QString sourceId;
    QString operation;
    QString file;
    QStringList path;
    qsizetype line = 0;
    bool empty() const { return code.isEmpty(); }
};
struct ConfigBackendCapabilities {
    bool read = true;
    bool write = false;
    bool watch = false;
    bool conditionalPublish = false;
    // Operations are called by one owner thread; results contain only owned data.
    enum class Affinity { SingleOwner };
    Affinity affinity = Affinity::SingleOwner;
};
struct ConfigRevision {
    bool exists = false;
    QByteArray token;
    bool operator==(const ConfigRevision &other) const {
        return exists == other.exists && token == other.token;
    }
    bool operator!=(const ConfigRevision &other) const { return !(*this == other); }
};
struct ConfigBackendRequest {
    enum class Operation { Read, Publish };
    Operation operation = Operation::Read;
    QString sourceId;
    QByteArray bytes;
    ConfigRevision expected;
};
struct ConfigBackendResult {
    quint64 requestId = 0;
    QString sourceId;
    ConfigBackendRequest::Operation operation = ConfigBackendRequest::Operation::Read;
    bool success = false;
    ConfigRevision revision;
    QByteArray bytes;
    enum class PublicationState { Known, Uncertain };
    PublicationState publicationState = PublicationState::Known;
    ConfigError error;
};

class ConfigBackend {
public:
    virtual ~ConfigBackend() = default;
    virtual ConfigBackendCapabilities capabilities() const = 0;
    // Register at submit, consume each completion once. No consumer callback/reentry.
    // Caller keeps this backend alive until its operations settle; no hidden retry.
    virtual quint64 submit(const ConfigBackendRequest &request) = 0;
    virtual std::optional<ConfigBackendResult> take(quint64 requestId) = 0;
};
} // namespace configqml
Q_DECLARE_METATYPE(configqml::ConfigError)
