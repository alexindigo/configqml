// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlstore.h"
#include "configqmlvalue.h"
#include "configconsumer_p.h"
#include "configqmlmodel.h"
#include <QQmlEngine>
#include <QTimer>

namespace configqml {
void ConfigQmlStore::setSchema(ConfigQmlSchema *schema) {
    if (m_complete) {
        reject({}, "schema-is-construction-only");
        return;
    }
    m_schema = schema;
    ConfigConsumerGuard(this).invoke([&] { emit schemaChanged(); });
}
ConfigQmlStore::Status ConfigQmlStore::status() const {
    return m_core ? Status(m_core->status()) : m_error.isEmpty() ? Loading : Error;
}
void ConfigQmlStore::componentComplete() {
    m_complete = true;
    QList<ConfigDiagnostic> errors;
    auto descriptor = m_schema ? m_schema->compile(errors) : ConfigDescriptor::group({});
    ConfigStoreOptions options;
    options.modelNumbers = ConfigListModel::Numbers::DecimalStrings;
    options.autoSave = m_autoSave;
    options.autoSaveInterval = m_autoSaveInterval;
    options.schema = ConfigSchema(descriptor);
    if (!m_schema)
        errors.append(ConfigDiagnostic{{}, "missing-schema"});
    errors.append(options.schema.errors());
    for (auto *source : m_sources) {
        const auto *file = qobject_cast<ConfigJsonFileSource *>(source);
        auto raw = captureQmlValue(source->data, source->prefix);
        errors.append(raw.errors);
        // Raw exact integer source values may be native JSON tokens; MemorySource
        // accepts QML strings for declared exact domains, without normalizing unknown keys.
        auto tree = raw.value.toMap();
        for (const auto &path : options.schema.leaves()) {
            if (file)
                break;
            if (!startsWith(path, source->prefix))
                continue;
            const auto relative = path.mid(source->prefix.size());
            auto value = valueAt(tree, relative);
            if (!value)
                continue;
            const auto checked = admitQmlValue(*options.schema.descriptor(path), *value, path);
            if (checked.ok())
                putValue(tree, relative, checked.value);
            else
                errors.append(checked.errors);
        }
        int index = -1;
        for (qsizetype i = 0; i < options.layers.size(); ++i)
            if (options.layers[i].id == source->layerId)
                index = i;
        if (index < 0) {
            index = options.layers.size();
            options.layers.append({source->layerId,
                                   ConfigLayerKind(source->kind),
                                   file ? ConfigAdmissionPolicy(file->admissionPolicy)
                                        : ConfigAdmissionPolicy::FieldFallback,
                                   {},
                                   source->writable});
        } else if (options.layers[index].kind != ConfigLayerKind(source->kind) ||
                   options.layers[index].writable != source->writable ||
                   options.layers[index].policy !=
                       (file ? ConfigAdmissionPolicy(file->admissionPolicy)
                             : ConfigAdmissionPolicy::FieldFallback))
            errors.append(ConfigDiagnostic{{}, "inconsistent-layer"});
        options.layers[index].sources.append(
            {source->sourceId, source->prefix, tree, {}, {}, source->exists});
        if (file) {
            if (source->kind != ConfigMemorySource::File || !source->data.isEmpty())
                errors.append(ConfigDiagnostic{source->prefix, "invalid-json-source"});
            options.jsonFiles.append({source->layerId, source->sourceId, file->path, file->optional,
                                      ConfigWatchMode(file->watchMode)});
        }
    }
    if (!errors.isEmpty()) {
        m_error = errors.first().code;
        QTimer::singleShot(
            0, this, [this] { ConfigConsumerGuard(this).invoke([&] { emit statusChanged(); }); });
        return;
    }
    m_core = new ConfigStore(options, this);
    m_values = createConfigNamespace(this, *m_core->schema().descriptor({}), {}, this);
    connect(m_core, &ConfigStore::statusChanged, this,
            [this] { ConfigConsumerGuard(this).invoke([&] { emit statusChanged(); }); });
    connect(m_core, &ConfigStore::completed, this, [this](const auto &result) {
        ConfigConsumerGuard(this).invoke([&] { emit completed(qmlResult(result)); });
    });
    connect(m_core, &ConfigStore::autoSaved, this, [this](const auto &result) {
        ConfigConsumerGuard(this).invoke([&] { emit autoSaved(qmlResult(result)); });
    });
    connect(m_core, &ConfigStore::sourceError, this, [this](const auto &error) {
        const QVariantMap value{{"code", error.code},       {"sourceId", error.sourceId},
                                {"phase", error.operation}, {"file", error.file},
                                {"path", error.path},       {"line", error.line}};
        ConfigConsumerGuard(this).invoke([&] { emit sourceError(value); });
    });
    connect(m_core, &ConfigStore::inputRejected, this, [this](const auto &path, const auto &code) {
        ConfigConsumerGuard(this).invoke([&] { emit inputRejected(path, code); });
    });
    ConfigConsumerGuard(this).invoke([&] { emit valuesChanged(); });
}
void ConfigQmlStore::reject(const ConfigPath &path, const QString &code) {
    ConfigConsumerGuard boundary(this);
    if (!boundary.invoke([&] { emit inputRejected(path, code); }))
        return;
    boundary.invoke([&] { raise(code); });
}
void ConfigQmlStore::raise(const QString &code) {
    if (auto *engine = qjsEngine(this))
        engine->throwError(QJSValue::TypeError, QStringLiteral("configqml: ") + code);
}
ConfigStore::Callback ConfigQmlStore::callback(const QJSValue &function) {
    if (!function.isCallable())
        return {};
    QPointer<ConfigQmlStore> guard(this);
    return [guard, function](const ConfigResult &result) {
        if (!guard)
            return;
        if (auto *engine = qjsEngine(guard)) {
            ConfigConsumerGuard boundary(guard, engine);
            QJSValue returned;
            if (!boundary.invoke(
                    [&] { returned = function.call({engine->toScriptValue(qmlResult(result))}); }))
                return;
            if (returned.isError())
                qWarning("configqml: completion callback threw");
        }
    };
}
QVariant ConfigQmlStore::read(const ConfigPath &path) const {
    if (!m_core)
        return {};
    return m_core->presentation(path);
}
ConfigRequestHandle ConfigQmlStore::write(const ConfigPath &path, const QVariant &value,
                                          const QJSValue &function) {
    ConfigConsumerGuard boundary(this);
    if (!m_core) {
        reject(path, "not-ready");
        return {};
    }
    const auto schema = m_core->schema();
    ConfigAdmission checked;
    if (!boundary.invoke([&] { checked = admitQmlInput(schema, path, value); }))
        return {};
    ConfigRequestHandle handle;
    if (!boundary.invoke([&] { handle = m_core->setUser(checked, this, callback(function)); }))
        return handle;
    if (!handle.accepted)
        boundary.invoke([&] {
            if (auto *engine = qjsEngine(this))
                engine->throwError(QJSValue::TypeError,
                                   QStringLiteral("configqml: ") + handle.errors.first().code);
        });
    return handle;
}
QString ConfigQmlStore::setUser(const QStringList &p, const QVariant &v, const QJSValue &f) {
    return QString::number(write(p, v, f).requestId);
}
QString ConfigQmlStore::setRuntime(const QStringList &p, const QVariant &v, const QJSValue &f) {
    ConfigConsumerGuard boundary(this);
    if (!m_core) {
        reject(p, "not-ready");
        return {};
    }
    const auto schema = m_core->schema();
    ConfigAdmission value;
    if (!boundary.invoke([&] { value = admitQmlInput(schema, p, v); }))
        return {};
    ConfigRequestHandle result;
    if (!boundary.invoke([&] { result = m_core->setRuntime(value, this, callback(f)); }))
        return QString::number(result.requestId);
    if (!result.accepted)
        boundary.invoke([&] { raise(result.errors.first().code); });
    return QString::number(result.requestId);
}
QString ConfigQmlStore::reset(const QStringList &p, const QString &t, const QJSValue &f) {
    ConfigConsumerGuard boundary(this);
    if (!m_core) {
        reject(p, "not-ready");
        return {};
    }
    ConfigRequestHandle result;
    if (!boundary.invoke([&] { result = m_core->reset(p, t, this, callback(f)); }))
        return QString::number(result.requestId);
    if (!result.accepted)
        reject(p, result.errors.first().code);
    return QString::number(result.requestId);
}
QString ConfigQmlStore::clearRuntime(const QStringList &p, const QJSValue &f) {
    ConfigConsumerGuard boundary(this);
    if (!m_core) {
        reject(p, "not-ready");
        return {};
    }
    ConfigRequestHandle result;
    if (!boundary.invoke([&] { result = m_core->clearRuntime(p, this, callback(f)); }))
        return QString::number(result.requestId);
    if (!result.accepted)
        reject(p, result.errors.first().code);
    return QString::number(result.requestId);
}
QVariantMap ConfigQmlStore::info(const QStringList &p) const {
    if (!m_core)
        return {};
    const auto info = m_core->info(p);
    return {{"declared", info.provenance.declared},
            {"present", info.provenance.present},
            {"sourceId", info.provenance.sourceId},
            {"layerId", info.provenance.layerId},
            {"maskedBy", info.provenance.maskedBy},
            {"description", info.provenance.maskedBy.isEmpty()
                                ? QString()
                                : QString("masked by ") + info.provenance.maskedBy},
            {"latestInput", QString::number(info.latestInput)},
            {"workerCovered", QString::number(info.workerCovered)},
            {"pathGeneration", QString::number(info.pathGeneration)},
            {"route", QVariantMap{{"layerId", info.provenance.route.layerId},
                                  {"sourceId", info.provenance.route.sourceId}}}};
}
QAbstractItemModel *ConfigQmlStore::listModel(const QStringList &queryPath) {
    const auto path = ownPath(queryPath);
    auto *source = m_core ? m_core->listModel(path) : nullptr;
    if (!source) {
        reject(path, "not-a-list-setting");
        return nullptr;
    }
    if (m_models.value(path))
        return m_models.value(path);
    QQmlEngine::setObjectOwnership(source, QQmlEngine::CppOwnership);
    m_models[path] = source;
    connect(source, &ConfigListModel::writeRejected, this,
            [this, path] { reject(path, "read-only-model"); });
    return source;
}
QAbstractItemModel *ConfigQmlStore::keyedModel(const QStringList &queryPath) {
    const auto path = ownPath(queryPath);
    auto *source = m_core ? m_core->keyedModel(path) : nullptr;
    if (!source) {
        reject(path, "not-a-map-setting");
        return nullptr;
    }
    if (!m_models.value(path)) {
        QQmlEngine::setObjectOwnership(source, QQmlEngine::CppOwnership);
        m_models[path] = source;
        connect(source, &ConfigListModel::writeRejected, this,
                [this, path] { reject(path, "read-only-model"); });
    }
    return m_models.value(path);
}
QString ConfigQmlStore::save(const QJSValue &f) {
    if (!m_core) {
        reject({}, "not-ready");
        return {};
    }
    quint64 requestId = 0;
    ConfigConsumerGuard(this).invoke([&] { requestId = m_core->save(this, callback(f)); });
    return QString::number(requestId);
}
QString ConfigQmlStore::reload(const QJSValue &f) {
    if (!m_core) {
        reject({}, "not-ready");
        return {};
    }
    quint64 id = 0;
    ConfigConsumerGuard(this).invoke([&] { id = m_core->reload(this, callback(f)); });
    return QString::number(id);
}
QString ConfigQmlStore::initializeIfMissing(const QStringList &p, const QVariant &v,
                                            const QJSValue &f) {
    if (!m_core) {
        reject(p, "not-ready");
        return {};
    }
    const auto schema = m_core->schema();
    ConfigAdmission checked;
    ConfigConsumerGuard boundary(this);
    if (!boundary.invoke([&] { checked = admitQmlInput(schema, p, v); }))
        return {};
    ConfigRequestHandle handle;
    if (!boundary.invoke([&] { handle = m_core->initializeIfMissing(checked, this, callback(f)); }))
        return QString::number(handle.requestId);
    if (!handle.accepted)
        boundary.invoke([&] { raise(handle.errors.first().code); });
    return QString::number(handle.requestId);
}
QString ConfigQmlStore::flush(const QJSValue &f) {
    if (!m_core) {
        reject({}, "not-ready");
        return {};
    }
    quint64 requestId = 0;
    ConfigConsumerGuard(this).invoke([&] { requestId = m_core->flush(this, callback(f)); });
    return QString::number(requestId);
}
QString ConfigQmlStore::close(const QJSValue &f) {
    if (!m_core) {
        reject({}, "not-ready");
        return {};
    }
    quint64 requestId = 0;
    ConfigConsumerGuard(this).invoke([&] { requestId = m_core->close(this, callback(f)); });
    return QString::number(requestId);
}
} // namespace configqml
