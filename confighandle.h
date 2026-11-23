// SPDX-License-Identifier: LGPL-3.0-or-later
// Schema-generated handles over existing owners. Every verb returns one pending
// reply; P1 selection, P2 edits and P3 observation stay with their native owners.
#pragma once
#include "configpendingreply.h"
#include "configschema.h"
#include <QObject>
#include <optional>

namespace configqml {
class ConfigHandle;
class ConfigHandleState;
struct ConfigHandleOptions {
    // Additional exact read handles acknowledged after the operation. The
    // initiating handle is always observed first.
    QList<ConfigHandle *> observe;
};
struct ConfigHandleEdit {
    ConfigPath path; // Relative to the handle; omitted leaves are not edited.
    QVariant value;
};
class ConfigHandle : public QObject {
    Q_OBJECT
public:
    enum class Kind { Live, Staged, ReadOnly };
    Q_ENUM(Kind)
    ~ConfigHandle() override;
    Kind kind() const;
    bool valid() const;
    QString error() const;
    QString health() const;
    ConfigPath path() const;
    bool isField() const;
    QStringList scopes() const;
    QStringList fields() const;
    QStringList capabilities() const;
    std::optional<ConfigDescriptor> descriptor() const;
    ConfigHandle *scope(const ConfigPath &relative);
    ConfigHandle *select(const QList<ConfigPath> &relative);
    ConfigHandle *preview();
    bool present() const;
    QVariant value() const;
    ConfigSelectedState state() const;
    ConfigReadSnapshot snapshot() const;
    ConfigObservation generation() const;
    ConfigPendingReply *read(const ConfigHandleOptions &options = {});
    ConfigPendingReply *set(const QVariant &value, const ConfigHandleOptions &options = {});
    ConfigPendingReply *set(const QList<ConfigHandleEdit> &batch,
                            const ConfigHandleOptions &options = {});
    ConfigPendingReply *remove(const ConfigHandleOptions &options = {});
    ConfigPendingReply *unset(const ConfigHandleOptions &options = {});
    ConfigPendingReply *apply(const ConfigHandleOptions &options = {});
    ConfigPendingReply *discard(const ConfigHandleOptions &options = {});
    ConfigPendingReply *observe(const ConfigObservation &after = {},
                                const ConfigHandleOptions &options = {});
    ConfigPendingReply *whenCurrent(const ConfigObservation &after = {},
                                    const ConfigHandleOptions &options = {});
    ConfigPendingReply *edit(const ConfigHandleOptions &options = {});
    // Staged draft handles: abandon this session's failed admitted Apply.
    ConfigPendingReply *abandon(const ConfigHandleOptions &options = {});
signals:
    // Exactly once per actual effective transition of a field (or any selected
    // field of a scope). Metadata-only and no-op publications emit stateChanged.
    void valueChanged();
    void stateChanged();

private:
    friend class ConfigHandleState;
    explicit ConfigHandle(QObject *parent);
    std::unique_ptr<ConfigHandleState> d;
};
} // namespace configqml
Q_DECLARE_METATYPE(configqml::ConfigHandle *)
