// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configselection_p.h"
#include "confighandle.h"
#include "configpendingreply_p.h"
#include <QHash>
#include <QPointer>

namespace configqml {
class ConfigStore;
class ConfigStoreHandle;
class ConfigDraft;
class ConfigOverlayEditor;
class ConfigReadProjection;
// Immutable schema identity beneath generated handles. This describes a selected
// namespace only; P1/P2/P3 retain all value, candidate and observation ownership.
class ConfigHandleSelection {
public:
    static ConfigHandleSelection capture(const ConfigSelection &selection,
                                         const ConfigPath &prefix = {});
    bool valid() const { return m_selection.valid(); }
    QString error() const { return m_selection.error; }
    ConfigPath prefix() const { return m_prefix; }
    ConfigSelection selection() const { return m_selection; }
    ConfigHandleSelection scope(const ConfigPath &relative) const;
    ConfigHandleSelection select(const QList<ConfigPath> &relative) const;
    QStringList scopes() const;
    QStringList fields() const;
    std::optional<ConfigDescriptor> descriptor() const;

private:
    ConfigHandleSelection() { m_selection.error = "handle-selection"; }
    ConfigHandleSelection restrict(const QList<ConfigPath> &absolute,
                                   const ConfigPath &prefix) const;
    QStringList children(bool groups) const;
    ConfigSelection m_selection;
    ConfigPath m_prefix;
};
// Private state of one generated handle. Roots belong to an existing owner (or,
// for an opened edit, own the draft they adapt); children belong to their root.
// No value, candidate, persistence or capture is owned here: every verb is
// dispatched to the owner's existing P1/P2/P3 path and composed by one reply.
class ConfigHandleState {
public:
    enum class Owner { Store, Scope, Draft, Preview, Overlay };
    static ConfigHandle *root(ConfigStore *store, bool runtime);
    static ConfigHandle *root(ConfigStoreHandle *scope);
    static ConfigHandle *root(ConfigDraft *draft, bool preview);
    static ConfigHandle *root(ConfigOverlayEditor *overlay);
    static ConfigHandleState *get(const ConfigHandle *handle) { return handle->d.get(); }
    static ConfigPendingReply *set(ConfigHandle *handle, const QList<ConfigHandleEdit> &batch,
                                   const ConfigHandleOptions &options,
                                   ConfigDescriptor::Format format);
    ConfigSchema schema() const;
    ConfigPath ownerPath(const ConfigPath &absolute) const;

private:
    friend class ConfigHandle;
    explicit ConfigHandleState(ConfigHandle *handle);
    static ConfigHandle *create(QObject *parent, ConfigHandleState *root,
                                const ConfigHandleSelection &selection);
    static ConfigHandle *makeRoot(QObject *owner, const QString &name, Owner kind,
                                  const ConfigHandleSelection &selection);
    static ConfigHandle *open(ConfigDraft *draft, const ConfigHandleSelection &selection);
    void wire();
    bool alive() const;
    bool writable() const;
    bool frozen() const;
    ConfigReadProjection *ownerProjection() const;
    ConfigReadProjection *projection();
    ConfigPath ownerPrefix() const;
    QList<ConfigPath> ownerLeaves() const;
    QVariant current(bool &present) const;
    void prime();
    void refresh(const QList<ConfigPath> &paths, bool all);
    void metadata();
    ConfigPendingReply *start(ConfigReplyWriter writer, bool barrier,
                              const ConfigObservation &after, const ConfigHandleOptions &options,
                              ConfigHandle *opened = nullptr);
    ConfigPendingReply *refuse(const QString &code, const QString &phase) const;
    ConfigPendingReply *remove(const ConfigHandleOptions &options);
    ConfigPendingReply *apply(const ConfigHandleOptions &options);
    ConfigPendingReply *discard(const ConfigHandleOptions &options);
    ConfigPendingReply *observe(const ConfigObservation &after, const ConfigHandleOptions &options);
    ConfigPendingReply *edit(const ConfigHandleOptions &options);
    ConfigPendingReply *abandon(const ConfigHandleOptions &options);
    bool coversOwner() const;

    ConfigHandle *q;
    ConfigHandleState *rootState = nullptr;
    ConfigHandleSelection selection;
    Owner owner = Owner::Store;
    ConfigHandle::Kind kind = ConfigHandle::Kind::ReadOnly;
    bool runtime = false, field = false;
    QPointer<ConfigStore> store;
    QPointer<ConfigStoreHandle> scope;
    QPointer<ConfigDraft> draft;
    QPointer<ConfigOverlayEditor> overlay;
    QPointer<ConfigReadProjection> selected;
    QPointer<ConfigHandle> previewHandle;
    // Root only: cached lookups and materialized handles notified by path.
    QHash<QString, QPointer<ConfigHandle>> lookups;
    QList<QPointer<ConfigHandle>> materialized;
    QString health = "ok";
    bool cached = false, cachedPresent = false;
    QVariant cachedValue;
};
} // namespace configqml
