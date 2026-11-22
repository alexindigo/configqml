// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configreadprojection.h"
#include "configobservation_p.h"
#include <QPointer>

namespace configqml {
class ConfigStore;
class ConfigStoreHandle;
class ConfigOverlayEditor;
class ConfigDraft;
class ConfigHandleSelection;
struct ConfigOverlayPresentation;
// One nestable notification fence for local ingress and worker publications.
// QPointer storage, not a reference into an emitter that a handler may delete.
class ConfigPublicationScope {
public:
    explicit ConfigPublicationScope(ConfigStore *store);
    ~ConfigPublicationScope();
    ConfigPublicationScope(const ConfigPublicationScope &) = delete;
    ConfigPublicationScope &operator=(const ConfigPublicationScope &) = delete;

private:
    QPointer<ConfigStore> m_store;
    bool m_previous = false;
};
class ConfigReadProjectionState {
public:
    static ConfigReadProjection *get(ConfigStore *store);
    static ConfigReadProjection *get(ConfigStoreHandle *handle);
    static ConfigReadProjection *get(ConfigOverlayEditor *overlay);
    static ConfigReadProjection *get(ConfigDraft *draft, bool preview);
    static ConfigReadProjection *select(ConfigReadProjection *source,
                                        const ConfigHandleSelection &selection, QObject *owner);
    static void initialize(ConfigOverlayEditor *overlay, const ConfigReadCapture &capture);
    static ConfigOverlayPresentation eligible(ConfigOverlayEditor *overlay,
                                              const ConfigOverlayPresentation &presentation);
    static void install(ConfigStore *store, const ConfigWorkerPacket &packet);
    static bool complete(ConfigStore *store);
    static bool deliver(ConfigStore *store);
    static bool operation(ConfigReadProjection *projection, const ConfigCompletion &pair);
    static void refresh(ConfigDraft *draft);
    static void stage(ConfigOverlayEditor *overlay);
    static void stage(ConfigDraft *draft);
    static bool available(ConfigReadProjection *projection);

private:
    friend class ConfigReadProjection;
    explicit ConfigReadProjectionState(ConfigReadProjection *projection);
    static ConfigReadProjection *create(QObject *owner, ConfigStore *store, const QString &name);
    bool live() const;
    ConfigReadProjectionState *origin();
    QList<ConfigPath> pending(quint64 covered) const;
    void receive(const ConfigReadCapture &capture);
    bool prepare();
    bool finish();
    void fail(quint64 requestId, const QString &error);
    void invalidate();
    ConfigReadProjection *q;
    QPointer<ConfigStore> store;
    QPointer<ConfigStoreHandle> handle;
    QPointer<ConfigOverlayEditor> overlay;
    QPointer<ConfigDraft> draft;
    QPointer<ConfigReadProjection> source;
    std::shared_ptr<std::atomic_bool> sourceLifetime, selectedPreviewLifetime;
    bool preview = false, frozen = false;
    ConfigPath prefix;
    QList<ConfigPath> paths;
    QString id;
    std::shared_ptr<std::atomic_bool> lifetime;
    quint64 next = 0, order = 0, coveredSequence = 0;
    quint64 metadataOrder = 0;
    quint64 editEpoch = 0;
    ConfigReadSnapshot installed, completed;
    bool changed = false;
    struct Pending {
        ConfigObservation after;
        QPointer<QObject> context;
        ConfigReadProjection::ReadCallback callback;
        bool captured = false;
        QString error;
    };
    QMap<quint64, Pending> requests;
    QList<QPointer<ConfigReadProjection>> views;
};
} // namespace configqml
