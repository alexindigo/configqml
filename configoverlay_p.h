// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configoverlayeditor.h"
#include <QMutex>
#include <atomic>

namespace configqml {
class ConfigWorker;
// Value-free transport/lifetime endpoint. No consumer QObject is sent to workers.
struct ConfigOverlayEndpoint {
    quint64 order = 0;
    std::function<bool(std::function<void(ConfigWorker &)>)> send;
    std::atomic_bool live{true};
    mutable QMutex mutex;
    QMap<ConfigPath, quint64> ingress;
    QMap<QString, std::weak_ptr<ConfigOverlayEndpoint>> dependencies;
    quint64 revision(const ConfigPath &path) const;
    void advance(const QList<ConfigPath> &paths);
};
struct ConfigOverlaySelection {
    ConfigOverlayMode mode = ConfigOverlayMode::Editor;
    std::shared_ptr<ConfigOverlayEndpoint> owner, base;
    std::shared_ptr<std::atomic_bool> baseLifetime;
    ConfigPath basePath, ownerPath;
    quint64 ownerGeneration = 0;
    QList<ConfigValueSelector> entryPath, identityPath;
    QVariant identityValue;
    QString overrideKey;
};
struct ConfigOverlayPresentation {
    QString editorId, error;
    ConfigOverlayMode mode = ConfigOverlayMode::Editor;
    ConfigSchema schema{ConfigDescriptor::group({})};
    QMap<ConfigPath, QVariant> values;
    ConfigPath ownerPath;
    bool conflicted = false, refreshed = false;
};
struct ConfigOverlayCapture {
    ConfigOverlaySelection selection;
    ConfigSchema schema{ConfigDescriptor::group({})};
    QVariantMap baseValues, overrides;
    QVariant ownerValue;
    QList<ConfigValueSelector> rawEntryPath;
    QMap<ConfigPath, ConfigPathState> baseFences;
    ConfigPathState ownerFence;
    QMap<ConfigPath, quint64> baseIngress;
    quint64 ownerIngress = 0;
};
struct ConfigOverlayTransaction {
    // The ordered lease messages pass one continuation baton. Only its holder
    // mutates captures/error/acquired; cancellation from other threads sets this
    // atomic flag and releases local queued/paused batons through messages.
    QString token, editorId, operation;
    quint64 requestId = 0;
    ConfigOverlaySelection selection;
    std::optional<ConfigOverlayCapture> original;
    ConfigOverlayCapture captured;
    QMap<ConfigPath, std::optional<QVariant>> edits;
    QString error;
    QList<std::shared_ptr<ConfigOverlayEndpoint>> order;
    qsizetype acquired = 0;
    std::atomic_bool cancelled{false};
};
struct ConfigOverlaySubscription {
    std::weak_ptr<ConfigOverlayEndpoint> endpoint;
    QString editorId;
    QList<ConfigPath> paths;
    QMap<ConfigPath, quint64> revisions;
    std::shared_ptr<std::atomic_bool> lifetime;
};
void overlayRemoveDependency(const std::shared_ptr<ConfigOverlayEndpoint> &endpoint,
                             const QString &id);
// Pure worker-side declared policy. Schema values and opaque raw remain distinct.
QVariant overlayMerge(const QVariant &base, const QVariant &overrides);
QVariantMap overlayDifference(const ConfigSchema &schema, const QVariantMap &base,
                              const QVariantMap &edited, const QVariantMap &original);
std::optional<QVariant> overlayAt(const QVariant &value, const QList<ConfigValueSelector> &path);
std::optional<QVariant> overlayReplace(const QVariant &value,
                                       const QList<ConfigValueSelector> &path,
                                       const QVariant &replacement);
std::optional<ConfigDescriptor> overlayDescriptor(ConfigDescriptor descriptor,
                                                  const QList<ConfigValueSelector> &path);
QList<ConfigValueSelector>
overlayRawAddress(const QVariant &raw, const QList<ConfigValueSelector> &displayAddress,
                  const QList<ConfigRecordListPolicySpec> &policies,
                  const QMap<QString, ConfigRecordListMetadata> &metadata, QString &error);
} // namespace configqml
