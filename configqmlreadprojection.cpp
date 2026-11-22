// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlreadprojection.h"
#include "configqmlstore.h"
#include "configqmloverlay.h"
#include "configqmlevent_p.h"
#include "configreadprojection_p.h"
#include "configstorehandle.h"
#include "configconsumer_p.h"
#include <QQmlEngine>

namespace configqml {
QJSValue qmlSelectedStateEvent(QJSEngine *engine, const ConfigSelectedState &state,
                               const std::optional<ConfigDescriptor> &descriptor) {
    return qmlEvent(engine,
                    {"valid", "present", "value", "targetPresent", "pending", "authoredPresent",
                     "authoredValue", "candidateAvailable", "candidatePresent", "candidateValue",
                     "maskedBy", "observation"},
                    [state, descriptor](const QString &key) -> QVariant {
                        if (key == "value" || key == "authoredValue" || key == "candidateValue") {
                            const auto value = key == "value"           ? state.value()
                                               : key == "authoredValue" ? state.authoredValue()
                                                                        : state.candidateValue();
                            return descriptor && value.isValid() ? qmlValue(*descriptor, value)
                                                                 : QVariant();
                        }
                        if (key == "observation")
                            return QVariant::fromValue(state.observation());
                        if (key == "pending")
                            return state.pending() == ConfigSelectedState::Pending::Set ? "set"
                                   : state.pending() == ConfigSelectedState::Pending::Remove
                                       ? "remove"
                                       : "none";
                        return QVariantMap{{"valid", state.valid()},
                                           {"present", state.present()},
                                           {"targetPresent", state.targetPresent()},
                                           {"authoredPresent", state.authoredPresent()},
                                           {"candidateAvailable", state.candidateAvailable()},
                                           {"candidatePresent", state.candidatePresent()},
                                           {"maskedBy", state.maskedBy()}}
                            .value(key);
                    });
}
namespace {
QJSValue selectedEvent(QJSEngine *engine, const ConfigReadSnapshot &snapshot,
                       const ConfigPath &path) {
    return qmlSelectedStateEvent(engine, snapshot.selectedState(path),
                                 ConfigObservationAccess::schema(snapshot).descriptor(path));
}
class ReadSnapshot : public QObject {
    Q_OBJECT
public:
    ReadSnapshot(QJSEngine *engine, ConfigReadSnapshot snapshot)
        : m_engine(engine), m_snapshot(std::move(snapshot)) {}
    Q_INVOKABLE QJSValue state(const QStringList &path) const {
        return selectedEvent(m_engine, m_snapshot, path);
    }

private:
    QPointer<QJSEngine> m_engine;
    const ConfigReadSnapshot m_snapshot;
};
} // namespace
QJSValue qmlReadSnapshotEvent(QJSEngine *engine, const ConfigReadSnapshot &value) {
    if (!engine)
        return {};
    auto *snapshot = new ReadSnapshot(engine, value);
    QQmlEngine::setObjectOwnership(snapshot, QQmlEngine::JavaScriptOwnership);
    return engine->newQObject(snapshot);
}
QJSValue qmlReadResultEvent(QJSEngine *engine, const ConfigReadResult &result) {
    const QPointer<QJSEngine> guard(engine);
    return qmlEvent(engine, {"requestId", "status", "error", "observation", "snapshot", "pending"},
                    [guard, result](const QString &key) -> QVariant {
                        if (key == "requestId")
                            return QString::number(result.requestId());
                        if (key == "status")
                            return result.status();
                        if (key == "error")
                            return QVariant::fromValue(qmlErrorEvent(guard, result.error()));
                        if (key == "observation")
                            return QVariant::fromValue(result.observation());
                        if (key == "snapshot")
                            return QVariant::fromValue(
                                qmlReadSnapshotEvent(guard, result.snapshot()));
                        if (key == "pending") {
                            QVariantList pending;
                            for (const auto &path : result.pending())
                                pending.append(QVariant(path));
                            return pending;
                        }
                        return {};
                    });
}
ConfigQmlReadProjection::ConfigQmlReadProjection(ConfigReadProjection *projection,
                                                 QJSEngine *engine, QObject *parent)
    : QObject(parent) {
    QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
    bind(projection, engine);
}
void ConfigQmlReadProjection::bind(ConfigReadProjection *projection, QJSEngine *engine) {
    if (engine)
        m_engine = engine;
    if (m_projection == projection)
        return;
    if (m_projection)
        disconnect(m_projection, nullptr, this, nullptr);
    m_projection = projection;
    if (!projection)
        return;
    connect(projection, &ConfigReadProjection::stateChanged, this,
            &ConfigQmlReadProjection::stateChanged);
    connect(projection, &ConfigReadProjection::completed, this,
            [this](const ConfigReadResult &result) {
                const auto event = qmlReadResultEvent(m_engine, result);
                ConfigConsumerGuard(this, m_engine).invoke([&] { emit completed(event); });
            });
}
QJSValue ConfigQmlReadProjection::state(const QStringList &path) const {
    return selectedEvent(m_engine, m_projection ? m_projection->snapshot() : ConfigReadSnapshot(),
                         path);
}
QString ConfigQmlReadProjection::synchronize(const QJSValue &callback) {
    return synchronize(QVariant(), callback);
}
QString ConfigQmlReadProjection::synchronize(const QVariant &after, const QJSValue &callback) {
    if (!m_projection || !m_engine)
        return {};
    ConfigObservation token;
    if (after.isValid()) {
        if (after.metaType() != QMetaType::fromType<ConfigObservation>()) {
            m_engine->throwError(QJSValue::TypeError, "configqml: read-observation");
            return {};
        }
        token = after.value<ConfigObservation>();
    }
    const QPointer<ConfigQmlReadProjection> guard(this);
    return QString::number(
        m_projection->synchronize(token, this, [guard, callback](const ConfigReadResult &result) {
            if (!guard || !guard->m_engine || !callback.isCallable())
                return;
            const auto event = qmlReadResultEvent(guard->m_engine, result);
            ConfigConsumerGuard(guard, guard->m_engine).invoke([&] {
                const auto returned = callback.call({event});
                if (returned.isError())
                    qWarning("configqml: read callback threw");
            });
        }));
}
ConfigQmlReadProjection *ConfigQmlStore::readProjection() {
    auto *existing = findChild<ConfigQmlReadProjection *>("_configqml_qml_read_projection",
                                                          Qt::FindDirectChildrenOnly);
    ConfigReadProjection *native = nullptr;
    if (m_overlay)
        native = m_overlay->readProjection();
    else if (m_draft)
        native = m_draft->readProjection();
    else if (m_nativeScope)
        native = m_nativeScope->readProjection();
    else if (m_core && m_isHandle) {
        m_nativeScope = m_core->scope(m_prefix, m_writable, this);
        if (m_nativeScope)
            native = m_nativeScope->readProjection();
    } else if (m_core)
        native = m_core->readProjection();
    if (existing) {
        existing->bind(native, scriptEngine());
        return existing;
    }
    auto *result = new ConfigQmlReadProjection(native, scriptEngine(), this);
    result->setObjectName("_configqml_qml_read_projection");
    return result;
}
ConfigQmlReadProjection *ConfigQmlOverlay::readProjection() { return m_adapter->readProjection(); }
} // namespace configqml
#include "configqmlreadprojection.moc"
