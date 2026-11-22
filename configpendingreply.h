// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configobservation.h"
#include <QObject>

namespace configqml {
struct ConfigReplyErrorData;
struct ConfigReplyValueData;
class ConfigPendingReplyState;
class ConfigReplyError {
public:
    ConfigReplyError() = default;
    QString code() const;
    QString message() const;
    QString phase() const;
    bool empty() const;

private:
    friend class ConfigPendingReplyState;
    std::shared_ptr<const ConfigReplyErrorData> d;
};
class ConfigReplyValue {
public:
    ConfigReplyValue() = default;
    ConfigObservation generation() const;
    ConfigReadSnapshot snapshot() const;
    QList<ConfigReadResult> observations() const;
    QList<ConfigPath> pending() const;
    std::optional<ConfigResult> write() const;

private:
    friend class ConfigPendingReplyState;
    std::shared_ptr<const ConfigReplyValueData> d;
};
// Completion data is immutable and detached from the initiating presentation.
// Native callers own the returned object; the QML adapter gives it JS ownership.
class ConfigPendingReply : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isFinished READ isFinished NOTIFY finished)
    Q_PROPERTY(bool isError READ isError NOTIFY finished)
public:
    ~ConfigPendingReply() override;
    bool isFinished() const;
    bool isError() const;
    ConfigReplyError error() const;
    ConfigReplyValue value() const;
signals:
    void finished();

private:
    friend class ConfigPendingReplyState;
    explicit ConfigPendingReply(QObject *parent);
    std::unique_ptr<ConfigPendingReplyState> d;
};
} // namespace configqml
Q_DECLARE_METATYPE(configqml::ConfigReplyError)
Q_DECLARE_METATYPE(configqml::ConfigReplyValue)
