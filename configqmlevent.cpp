// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlevent_p.h"
#include <QJSEngine>
#include <QQmlEngine>

namespace configqml {
namespace {
class EventData : public QObject {
    Q_OBJECT
public:
    explicit EventData(std::function<QVariant(const QString &)> loader)
        : m_loader(std::move(loader)) {}
    Q_INVOKABLE QVariant read(const QString &key) const { return m_loader(key); }

private:
    const std::function<QVariant(const QString &)> m_loader;
};
class EventFactory : public QObject {
    Q_OBJECT
public:
    explicit EventFactory(QJSEngine *engine) : QObject(engine) {
        create = engine->evaluate(QStringLiteral(R"JS((function(data, keys) {
            const event = {};
            const define = Object.defineProperty;
            for (const key of keys) {
                define(event, key, {enumerable:true, configurable:true,
                    get:function() {
                        const value = data.read(key);
                        define(event, key, {value:value, enumerable:true,
                            configurable:true, writable:true});
                        return value;
                    },
                    set:function(value) {
                        define(event, key, {value:value, enumerable:true,
                            configurable:true, writable:true});
                    }});
            }
            return event;
        }))JS"));
    }
    QJSValue create;
};
QVariantList evidenceList(const QList<ConfigEvidenceReference> &references) {
    QVariantList values;
    for (const auto &ref : references)
        values.append(QVariantMap{{"evidenceId", ref.evidenceId},
                                  {"directory", ref.directory},
                                  {"sourceId", ref.sourceId},
                                  {"phase", ref.phase},
                                  {"role", ref.role},
                                  {"observationOrder", QString::number(ref.observationOrder)}});
    return values;
}
} // namespace
QJSValue qmlEvent(QJSEngine *engine, const QStringList &keys,
                  std::function<QVariant(const QString &)> loader) {
    if (!engine)
        return {};
    auto *factory = engine->findChild<EventFactory *>(QString(), Qt::FindDirectChildrenOnly);
    if (!factory)
        factory = new EventFactory(engine);
    auto *data = new EventData(std::move(loader));
    QQmlEngine::setObjectOwnership(data, QQmlEngine::JavaScriptOwnership);
    return factory->create.call({engine->newQObject(data), engine->toScriptValue(keys)});
}
QJSValue qmlResultEvent(QJSEngine *engine, const ConfigResult &result,
                        const ConfigDiagnosticBuilder &diagnostics) {
    return qmlEvent(engine, qmlResultKeys(), [result, diagnostics](const QString &key) {
        return qmlResultField(result, diagnostics, key);
    });
}
QJSValue qmlErrorEvent(QJSEngine *engine, const ConfigError &error) {
    return qmlEvent(engine, {"code", "sourceId", "phase", "file", "path", "line", "evidence"},
                    [error](const QString &key) -> QVariant {
                        if (key == "evidence")
                            return evidenceList(error.evidence);
                        return QVariantMap{{"code", error.code},       {"sourceId", error.sourceId},
                                           {"phase", error.operation}, {"file", error.file},
                                           {"path", error.path},       {"line", error.line}}
                            .value(key);
                    });
}
QJSValue qmlAcceptedEvent(QJSEngine *engine, const ConfigSourceAccepted &event,
                          const ConfigSchema &schema) {
    return qmlEvent(
        engine,
        {"eventId", "publicationGeneration", "inputPrefix", "sources", "changedPaths", "origin",
         "cause", "facts"},
        [event, schema](const QString &key) -> QVariant {
            if (key == "sources") {
                QVariantList values;
                for (const auto &source : event.sources)
                    values.append(
                        QVariantMap{{"sourceId", source.first},
                                    {"revision", QString::fromLatin1(source.second.token)}});
                return values;
            }
            if (key == "changedPaths") {
                QVariantList values;
                for (const auto &path : event.changedPaths)
                    values.append(QVariant(path));
                return values;
            }
            if (key == "facts") {
                QVariantList values;
                for (const auto &fact : event.facts) {
                    QVariantMap value{{"path", fact.path},
                                      {"present", fact.present},
                                      {"sourceId", fact.sourceId},
                                      {"revision", QString::fromLatin1(fact.revision.token)}};
                    if (fact.present)
                        value["value"] = qmlValue(*schema.descriptor(fact.path), fact.value);
                    values.append(value);
                }
                return values;
            }
            return QVariantMap{
                {"eventId", event.eventId},
                {"publicationGeneration", QString::number(event.publicationGeneration)},
                {"inputPrefix", QString::number(event.inputPrefix)},
                {"origin", event.origin},
                {"cause", event.cause}}
                .value(key);
        });
}
QJSValue qmlProjectionEvent(QJSEngine *engine, const ConfigProjectionEffectsEvent &event) {
    return qmlEvent(engine, {"runId", "projectionId", "sourceId", "revision", "effects"},
                    [event](const QString &key) -> QVariant {
                        if (key == "effects") {
                            QVariantList effects;
                            for (const auto &effect : event.effects)
                                effects.append(QVariantMap{{"effectId", effect.effectId},
                                                           {"version", double(effect.version)},
                                                           {"action", effect.action},
                                                           {"arguments", effect.arguments}});
                            return effects;
                        }
                        return QVariantMap{{"runId", event.runId},
                                           {"projectionId", event.projectionId},
                                           {"sourceId", event.sourceId},
                                           {"revision", event.revision}}
                            .value(key);
                    });
}
} // namespace configqml
#include "configqmlevent.moc"
