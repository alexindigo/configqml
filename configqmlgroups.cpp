// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlstore.h"
#include "configqmlvalue.h"
#include "configconsumer_p.h"

namespace configqml {
namespace {
bool keys(const QVariantMap &map, const QStringList &required, const QStringList &optional = {}) {
    for (const auto &key : required)
        if (!map.contains(key))
            return false;
    for (const auto &key : map.keys())
        if (!required.contains(key) && !optional.contains(key))
            return false;
    return true;
}
bool path(const QVariant &raw, ConfigPath &result) {
    if (raw.metaType().id() != QMetaType::QVariantList &&
        raw.metaType().id() != QMetaType::QStringList)
        return false;
    for (const auto &segment : raw.toList()) {
        if (segment.metaType().id() != QMetaType::QString)
            return false;
        result.append(segment.toString());
    }
    return true;
}
} // namespace
QString ConfigQmlStore::retainGroup(const QJSValue &options, const QJSValue &function) {
    ConfigConsumerGuard guard(this);
    const auto core = m_core;
    ConfigValueResult captured;
    if (!guard.invoke(
            [&] { captured = captureQmlValue(QVariant::fromValue(options), {}, m_diagnostics); }))
        return {};
    if (!captured.ok()) {
        reject({}, captured.errors.first().code);
        return {};
    }
    if (!core || core != m_core || !m_writable || m_draft) {
        reject({}, "not-ready");
        return {};
    }
    const auto record = captured.value.toMap();
    ConfigRetainedGroupOptions input;
    bool valid = captured.value.metaType().id() == QMetaType::QVariantMap &&
                 keys(record, {"requestKey", "paths", "edits"}, {"dependencies"}) &&
                 record.value("requestKey").metaType().id() == QMetaType::QString &&
                 record.value("paths").metaType().id() == QMetaType::QVariantList &&
                 record.value("edits").metaType().id() == QMetaType::QVariantList;
    input.requestKey = record.value("requestKey").toString();
    for (const auto &raw : record.value("paths").toList()) {
        ConfigPath selection = m_prefix;
        valid &= path(raw, selection);
        input.paths.append(selection);
    }
    if (record.contains("dependencies")) {
        valid &= record.value("dependencies").metaType().id() == QMetaType::QVariantList;
        for (const auto &dependency : record.value("dependencies").toList()) {
            valid &= dependency.metaType().id() == QMetaType::QString;
            input.dependencies.append(dependency.toString());
        }
    }
    for (const auto &raw : record.value("edits").toList()) {
        const auto edit = raw.toMap();
        const bool present = edit.value("present").toBool();
        valid &= raw.metaType().id() == QMetaType::QVariantMap &&
                 edit.value("present").metaType().id() == QMetaType::Bool &&
                 keys(edit, present ? QStringList{"path", "present", "value"}
                                    : QStringList{"path", "present"});
        ConfigGroupEdit item;
        item.path = m_prefix;
        valid &= path(edit.value("path"), item.path);
        item.present = present;
        item.value = edit.value("value");
        const auto descriptor = core->schema().descriptor(item.path);
        if (present && descriptor && descriptor->type != ConfigDescriptor::Type::Group) {
            const auto checked = admitQmlValue(*descriptor, item.value, item.path);
            if (!checked.ok()) {
                reject(item.path, checked.errors.first().code);
                return {};
            }
            item.value = checked.value;
        }
        input.edits.append(item);
    }
    if (!valid) {
        reject({}, "group-selection");
        return {};
    }
    quint64 id = 0;
    guard.invoke([&] { id = core->retainGroup(input, this, callback(function)); });
    return QString::number(id);
}
QVariantMap ConfigQmlStore::groupInfo(const QString &token) const {
    if (!m_core)
        return {{"error", QVariantMap{{"code", "group-token"}}}};
    return qmlGroupInfo(m_core->groupInfo(token), m_diagnostics);
}
QString ConfigQmlStore::saveGroup(const QString &token, const QJSValue &function) {
    if (!m_core || !m_writable || m_draft) {
        reject({}, "not-ready");
        return {};
    }
    quint64 id = 0;
    ConfigConsumerGuard(this).invoke(
        [&] { id = m_core->saveGroup(token, this, callback(function)); });
    return QString::number(id);
}
QString ConfigQmlStore::retryGroup(const QString &token, const QJSValue &function) {
    if (!m_core || !m_writable || m_draft) {
        reject({}, "not-ready");
        return {};
    }
    quint64 id = 0;
    ConfigConsumerGuard(this).invoke(
        [&] { id = m_core->retryGroup(token, this, callback(function)); });
    return QString::number(id);
}
QString ConfigQmlStore::releaseGroup(const QString &token, const QJSValue &function) {
    if (!m_core || !m_writable || m_draft) {
        reject({}, "not-ready");
        return {};
    }
    quint64 id = 0;
    ConfigConsumerGuard(this).invoke(
        [&] { id = m_core->releaseGroup(token, this, callback(function)); });
    return QString::number(id);
}
} // namespace configqml
