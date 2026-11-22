// SPDX-License-Identifier: LGPL-3.0-or-later
// Compatibility read capability; schema handles adapt this same capture owner.
#pragma once
#include "configobservation.h"
#include <QObject>
#include <functional>
#include <memory>

namespace configqml {
class ConfigReadProjectionState;
class ConfigReadProjection : public QObject {
    Q_OBJECT
public:
    using ReadCallback = std::function<void(const ConfigReadResult &)>;
    ~ConfigReadProjection() override;
    ConfigSelectedState selectedState(const ConfigPath &path) const;
    ConfigReadSnapshot snapshot() const;
    ConfigObservation observation() const;
    quint64 synchronize(const ConfigObservation &after = {}, QObject *context = nullptr,
                        ReadCallback callback = {});
signals:
    void stateChanged();
    void completed(const configqml::ConfigReadResult &result);
    void operationCompleted(const configqml::ConfigResult &result,
                            const configqml::ConfigObservation &observation);

private:
    friend class ConfigReadProjectionState;
    explicit ConfigReadProjection(QObject *parent);
    std::unique_ptr<ConfigReadProjectionState> d;
};
} // namespace configqml
